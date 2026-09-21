# Glamor COPY through Mesa draw: exact gates and coordinates

This note localizes the first Xorg 21.1.24 Present COPY through the Chrome9
Gallium software-T&L path.  It is specifically about the accepted one-texture
`copy_area` shader and does not broaden the native shader or KMS contracts.

## Measured split

The live `Xorg-pixels-v1` trace proves that each alternating 300x300 imported
source is changing and that the CPU sampler shadow is an exact copy: source
and shadow hashes match on every refresh, while the hashes change between
frames.  The observed arbitrary-byte nonzero count is about 58,000, so this is
not the static all-zero XWD payload.

The subsequent `Xorg-drawtrace-v2` and `Xorg-noemit-v3` traces show only six
submitted untextured `color+blend` triangles.  Triangles 0/1 cover the
1024x768 target and 2/3 and 4/5 cover 300x300 rectangles.  There is no
`textured`, `textured+blend`, or textured ioctl trace after the changing
sampler is bound.  The failure is therefore between post-VS vertex emission
and the textured ioctl call, not source production, DRI3 import, sampler
shadow refresh, command retirement, or KMS scanout.

## The vec2 varying has no Q contract

Glamor's `copy_area` vertex shader declares and computes `fill_pos` as a
`vec2`:

```glsl
gl_Position.xy = primitive.xy * v_matrix.xz + v_matrix.yw;
gl_Position.zw = vec2(0.0, 1.0);
fill_pos = (fill_offset + primitive.xy) * fill_size_inv;
```

The Chrome9 vbuf layout nevertheless requests `EMIT_4F` for the generic
texture coordinate.  The prior draw-time gate required finite Z and exact
W=1.0 even though the admitted nonprojective sampler and the UAPI consume only
S/T.

On the configured Mesa build, LLVM and `draw-use-llvm` are enabled by default.
When that default is in effect, the unused emitted components are exactly zero
in this path:

1. `draw_llvm_generate()` initializes its `outputs[][]` pointer table to
   null.
2. NIR `init_var_slots()` allocates output channels with
   `lp_build_alloca()`, which explicitly stores `LLVMConstNull`.
3. The vec2 `store_output` writes only its XY mask.
4. `convert_to_aos()` supplies literal zero for an absent output channel.

Thus the four-float vbuf record for glamor's varying is `(S,T,0,0)`, and an
exact `texcoord[3] == 1.0f` check rejects every triangle.  The non-LLVM TGSI
fallback is even less suitable as an API contract: `nir_to_tgsi` preserves
the XY write mask, but release `tgsi_exec_machine_run()` does not clear the
output array between executions, so unwritten Z/W are stale/indeterminate.

The correct bounded rule for a nonprojective 2D sample is therefore:

```text
position: finite X/Y/Z and finite positive reciprocal clip-W
texture coordinate: finite S and T
unused varying components: ignored
```

The independent positive `position.w` remains required because the kernel
UAPI sends it as RHW and the hardware uses it for perspective-correct varying
interpolation.  Ignoring an unused generic Q does not mean ignoring RHW.

There is one necessary classifier companion.  The current
`chrome9_fs_is_texture()` accepts an optional `nir_tex_src_projector` sourced
from coordinate W, while the current UAPI transmits only S/T.  Before dropping
the Z/W validation globally, the fixed REPLACE/MODULATE class must either:

* require `projector < 0` and `tex->num_srcs == 1`; or
* record a projective flag and keep projective forms rejected until a correct
  Q lowering/UAPI exists.

The first option is the narrow bring-up fix.  Apply the same rule to both
units of the two-texture class.  Position-RHW perspective correction remains
supported and is not a NIR texture projector.

Focused tests must include:

1. a one-texture REPLACE VS which stores only a vec2 generic with write mask
   `0x3`; require submitted `+2`, rejected `+0` for a quad and exact sampled
   pixels;
2. the same test with `DRAW_USE_LLVM=0`; its result must not depend on stale
   output channels;
3. a one-texture MODULATE vec2 varying;
4. two vec2 varyings for the two-texture program;
5. an otherwise matching NIR texture instruction with an explicit projector;
   require shader-classification rejection, not silently affine rendering;
6. the existing varying-position-RHW oracle unchanged, proving that relaxing
   generic Q did not remove perspective interpolation.

## Exact glamor geometry and coordinate flow

`present_execute_copy()` sends the source pixmap and window drawable through a
GC `CopyArea`.  `glamor_copy_fbo_fbo_draw()` builds each box in this order:

```text
(x1,y1), (x1,y2), (x2,y2), (x2,y1)
```

and calls `glamor_glDrawArrays_GL_QUADS`.  Native GL uses `GL_QUADS`; the GLES
helper uses a cached index buffer.  The draw module decomposes the quad into
the equivalent two triangles.  Chrome9's fallback QUADS decomposition is
`(0,1,3)` and `(1,2,3)`, exactly matching the two live rectangle triangles.

The VS uniforms implement:

```text
clip.x = pixel.x * (2 / target_width)  + offset_x * (2 / width) - 1
clip.y = pixel.y * (2 / target_height) + offset_y * (2 / height) - 1
s = (source_dx + pixel.x) / source_width
t = (source_dy + pixel.y) / source_height
```

Mesa draw performs homogeneous clipping, perspective divide and the Gallium
viewport before Chrome9 sees the vbuf.  `do_rhw_viewport()` stores screen X/Y/Z
and replaces position W with reciprocal clip-W.  The state tracker has already
normalized framebuffer orientation:

* `st_update_viewport()` applies its Y inversion when the framebuffer is
  `Y_0_TOP`;
* `st_update_scissor()` also converts to Gallium's top-origin exclusive box;
* the rasterizer state uses `half_pixel_center=1` and selects the bottom-edge
  rule from framebuffer orientation.

Chrome9 must not invert viewport Y or scissor Y again.  Its hardware scissor
is top-origin with exclusive max endpoints.  Screen coordinates are converted
to signed 16.16 and clamped to `[-0.5, extent-0.5]`; the upper clamp preserves
coverage of the final pixel center when a GL edge arrives exactly at the
target extent.  A glamor edge coordinate of 300 is valid on a 1024-wide root
target and is not clamped to 299.5.

## Every pre-submit exit

The following ordered exits can prevent a texture ioctl.

| layer | exit |
|---|---|
| `chrome9_draw_vbo` | recursive multi-draw dispatch; zero count/instances; indirect draw; absent/unsupported VS/FS; missing index mapping |
| draw frontend | vertex/index fetch bounds; VS execution; clip-volume removal; rasterizer discard; face culling; unsupported/empty primitive after decomposition |
| vbuf setup | missing POSITION/generic semantic; unexpected emitted stride; vertex allocation failure; invalid unmap min/max range |
| vbuf draw | array range outside emitted buffer; null element list; unsupported primitive |
| state gate | target/surface/format/guard; blend/alpha/depth/stencil capabilities; sampler count/state/view; texture format/layout/alias; scissor validity |
| constant gate | missing, short, unmapped, or nonfinite fragment constant buffer |
| per vertex | emitted index/capacity; nonfinite position; nonfinite packed gray; nonfinite or nonpositive RHW; nonfinite S/T; second-unit range gate |
| fixed conversion | all three vertices become collinear after 16.16 conversion/clamp; silently skipped |
| diagnostic throttle | `CHROME9_DRAW_LIMIT` reached |
| submission | selected DRM ioctl returns failure |

Draw clipping and culling happen before the vbuf callbacks.  Consequently a
`draw_vbo` entry with no vbuf draw callback does not implicate the kernel.
Likewise, the current zero-area return neither increments rejected triangles
nor prints a warning.

## One-run rejection instrumentation

If removing the invalid vec2 Z/W check still produces no textured submit, one
bounded run should log these checkpoints under an opt-in variable:

1. at `chrome9_draw_vbo`: sequence number, PID/context, mode, count,
   instances, indexed/indirect flags, FS class, target dimensions, rasterizer
   cull/scissor/discard flags;
2. at `get_vertex_info`: POSITION/COLOR/GENERIC/TEXCOORD source indices,
   texture count, emitted size and `layout_valid`;
3. at vbuf `draw_arrays`/`draw_elements`: primitive, start/count or first
   indices, emitted vertex count/stride;
4. at every `chrome9_emit_triangle` entry: source indices, proving the draw
   module did not clip/cull the primitive before the driver;
5. on every state/per-vertex exit: a distinct reason plus raw IEEE-754 words
   for position XYZRHW and each used ST pair;
6. on the degenerate exit: pre-clamp floats and final 16.16 XY cross product;
7. immediately before the ioctl: ioctl family, destination/source handles and
   layouts, scissor box, all three XY/RHW/ST tuples;
8. immediately after it: success or saved `errno` before another library call.

Do not reuse the context-wide warn-once bit for this trace; it hid the later
texture-coordinate rejection after an unrelated startup warning.  Bound the
trace by draw sequence and keep the first occurrence of every exit reason.
For the expected glamor COPY, the decisive signature is two nondegenerate
textured triangles, finite ST, positive RHW, a nonempty scissor intersecting
the rectangle, and one synchronous successful ioctl per triangle.

## Source anchors

The relevant Mesa 26.2.2 paths are:

* `src/gallium/drivers/chrome9/chrome9_draw.c`: vbuf layout, state gate,
  per-vertex gate, fixed conversion and ioctl selection;
* `src/gallium/auxiliary/draw/draw_vs_variant.c:94-117`: reciprocal-W
  viewport transform;
* `src/gallium/auxiliary/draw/draw_llvm.c`: output collection and AoS emit;
* `src/gallium/auxiliary/gallivm/lp_bld_nir_soa.c`: zero-initialized output
  slots and masked stores;
* `src/gallium/auxiliary/gallivm/lp_bld_flow.c:517-536`:
  `lp_build_alloca()` zero initialization;
* `src/mesa/state_tracker/st_atom_viewport.c:50-76` and
  `st_atom_scissor.c:45-107`: orientation conversion;
* Xorg 21.1.24 `glamor/glamor_copy.c:35-56,358-479` and
  `glamor/glamor_transform.c:36-105`: copy shader, quad and transform.
