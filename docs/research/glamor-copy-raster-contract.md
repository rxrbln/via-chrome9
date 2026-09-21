# Native glamor copy draw and zero-emission contract

## Result

The black window observed in the current Xorg run is not yet a bad Chrome9
texture packet.  The strongest live trace contains a valid sampler-view bind
but **no textured triangle submission at all**.  Xorg binds the source texture
before it tests each source/destination intersection and before it resolves the
destination FBO.  A sampler-bind diagnostic therefore does not prove that
`glDrawArrays` ran.

The captured `copy_area_` fragment shader is exactly the already implemented
one-texture REPLACE class.  Its `VARYING_SLOT_VAR0.xy` coordinate is accepted
by the Chrome9 classifier and routed by the draw module into the same two-float
D3 texture-coordinate slot used by the live proprietary Xorg oracle.  There is
no evidence for a new pixel-shader program or packet delta.

The next discriminator must instrument the Xorg-to-draw-module boundaries,
not alter the native packet.

## Provenance

The source-level findings below use Xorg server 21.1.24 from
`/tmp/xorg-server-21.1.24-src`, Mesa Chrome9 nested repository `27e647d`, and
the kernel Chrome9 nested repository `2e78ad0`.  The proprietary packet is the
live-proven stream in [xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).
Its Linux Xorg binary and the Windows D3D/OpenGL binaries are identified by
hash and symbol/virtual address in that note and
[windows-draw-path.md](windows-draw-path.md).

The relevant live files on the VX900 are:

```text
/root/tmp/chrome9/Xorg-noemit-v3.stdout
/root/tmp/chrome9/Xorg-generic-19972d4.stdout
/root/tmp/chrome9/Xorg-pixels-v1.stdout
/root/tmp/chrome9/glxgears-pixels-v1.log
```

## Exact modern glamor operation

Xorg 21.1.24 `glamor/glamor_copy.c:36-58` defines the plain CopyArea program:

```glsl
gl_Position.xy = primitive.xy * v_matrix.xz + v_matrix.yw;
gl_Position.zw = vec2(0.0, 1.0);
fill_pos = (fill_offset + primitive.xy) * fill_size_inv;
frag_color = texture(sampler, fill_pos);
```

`use_copyarea()` binds texture unit zero first and then uploads:

```text
fill_offset   = (dx, dy)
fill_size_inv = (1 / source_width, 1 / source_height)
```

For every destination box, `glamor_copy.c:433-441` writes vertices in this
order:

```text
(x1,y1), (x1,y2), (x2,y2), (x2,y1)
```

The desktop-GL path submits `GL_QUADS`; the compatibility helper decomposes a
quad into two indexed triangles when quads are unavailable
(`glamor_utils.h:725-732`).  The Chrome9 vbuf backend supports both direct and
indexed `MESA_PRIM_QUADS`, decomposing them as `(0,1,3)` and `(1,2,3)`
(`chrome9_draw.c:997-1001` and `1047-1053`).

### Viewport cancellation

For a destination tile of width `W`, height `H`, and drawable offset
`(ox,oy)`, `glamor_transform.c:45-103` uploads:

```text
v_matrix = (2/W, ox*2/W - 1, 2/H, oy*2/H - 1)
```

This CopyArea call requests neither drawable translation nor the half-pixel
point adjustment.  The same helper binds the FBO and calls
`glViewport(0,0,W,H)` through `glamor_pixmap.c:66-74`.  Gallium's usual
viewport scale/translate then cancels the NDC transform:

```text
screen_x = (W/2) * (primitive_x*2/W + ox*2/W - 1) + W/2
         = primitive_x + ox
screen_y = primitive_y + oy
```

The software draw module must therefore deliver window-space X/Y, `z=0`, and
`w=1` to the Chrome9 vbuf renderer.  The native Chrome9 A40--A52 viewport block
is still the established fixed immediate-vertex linkage; it must not be
replaced with the GL viewport merely because glamor used `glViewport` before
the software transform.

### Exact scissor and pre-draw gates

The source texture is bound by `glamor_use_program()` at
`glamor_copy.c:456` before the destination loop.  The loop computes:

```text
left   = max(-dx, source/destination bounds left)
top    = max(-dy, source/destination bounds top)
right  = min(-dx + source tile width, bounds right)
bottom = min(-dy + source tile height, bounds bottom)
```

At `glamor_copy.c:466-467`, `left >= right` or `top >= bottom` skips the
destination without issuing a GL draw.  At lines 469--472 a missing
destination FBO aborts after the source was already bound.  Only after both
checks does Xorg call `glScissor()` and
`glamor_glDrawArrays_GL_QUADS()` at lines 474--479.

The ends are exclusive.  This agrees with the proprietary Windows builder at
`VTGUModeDX32A.dll:0x101f7949` and the live 32x32 words:

```text
A80 = 80000020     color top=0, bottom=32
A81 = 81000020     color left=0, right=32
A82 = 82000020     scissor top=0, bottom=32
A83 = 83000020     scissor left=0, right=32
```

An empty box must remain a software no-op.  The current Mesa validator rejects
only `min > max`, not equality (`chrome9_draw.c:505-510`), and the kernel would
therefore receive a zero-area native scissor if such a draw reached vbuf.
Hardware behavior for that case is not established; do not use it as a
diagnostic or submit it deliberately.

### Cull, depth, and raster requirements

The Xorg 21.1.24 glamor tree contains no `GL_CULL_FACE`, `glCullFace`,
`glFrontFace`, `GL_DEPTH_TEST`, or `GL_RASTERIZER_DISCARD` state change.  The
copy relies on their GL defaults: disabled.  The current driver copies the
Gallium rasterizer state into draw and always requests the software pipeline,
which performs clipping, culling, flat-shade fixup, and primitive decomposition
before vbuf (`chrome9_context.c:125-139`, `chrome9_draw.c:69-78`).

The corresponding native state is independently exact:

```text
A01 = 01c01540     bit 7 clear: hardware culling disabled
A20 = 201c5c00     four-vertex triangle-fan reuse
A23 = 23000f02     triangle primitive, solid shading in vendor copy
```

The current structured triangle-list packet uses `A23=23004f02` for Gouraud
shading while retaining `A01=01c01540`.  A23 bit 8 is inert when A01 bit 7 is
clear.  REPLACE does not consume diffuse color, so solid versus Gouraud does
not alter this copy's fragment result.  Depth, stencil, fog, and alpha test are
disabled.  Enabling cull in the native packet cannot repair a draw which never
reached the ioctl.

## Fragment-class cross-check

`Xorg-generic-19972d4.stdout` captured this exact NIR:

```text
label: copy_area_
num_textures: 1
inputs_read: 32
textures_used: ...0001
samplers_used: 00000001
shader_in highp vec2 VARYING_SLOT_VAR0
%1 = load_deref(VAR0)
%2 = tex %1, texture0, sampler0, 2D
store gl_FragColor = %2
```

Mesa `chrome9_context.c:332-376` accepts either `VARYING_SLOT_TEX0+unit` or
`VARYING_SLOT_VAR0+unit` as a two-component texture coordinate.  The resulting
texture leaf maps to `CHROME9_FS_TEXTURE_REPLACE` at lines 773--780; enum value
7 is declared in `chrome9_context.h:24-38`.

The vbuf layout lookup independently tries vertex-shader `GENERIC0` and then
`TEXCOORD0`, and emits four floats for that coordinate slot
(`chrome9_draw.c:103-130`).  The final fixed layout for one texture is:

```text
position float4 | diffuse BGRA8 | specular BGRA8 | texture0 float4
```

Only S/T are copied into the UAPI vertex.  The glamor VS supplies `z=0,w=1`,
so the current finite-value and positive-W validation at
`chrome9_draw.c:659-667` is satisfied.  The generic varying is thus a software
linkage name, not a request for a different native fragment interpolator.

## Native proprietary packet contract

The live-proven proprietary Linux Xorg PictOpSrc operation is a 232-dword body
for a 32x32 XRGB copy.  Its exact stream and hash remain in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md); the state relevant to
this diagnosis is summarized here in emission order:

| body slots | native group | decisive words |
|---:|---|---|
| `0..15` | destination/blend | A52 `52980004`, ONE/ZERO blend closure |
| `16..27` | texture image 0 | A18 `18001100`, A20 `20020020`, A21 `21000001`, A22 `22008055`, A30 `30980000` |
| `28..35` | sampler 0 | A31 `31100000`, A32 `32000052` |
| `36..47` | common raster | A53 `53010c0f`, A00 `00000011`, A01 `01c01540` |
| `48..59` | texture linkage | A00 `00000011`, A01 `01002521`, A08 `08010101`, A05 `05000000`, A0D `0d810000` |
| `60..91` | FVF/primitive/native viewport | D3 `04006342`, fan `201c5c00`, A23 `23000f02`, A24 count 4, fixed A40--A52 block |
| `92..119` | PS/VS constants | zero PS c0; VS c256 `(1,1,1,1)` and c257 `(786432,786432,0,0)` |
| `120..127` | color/scissor windows | A80--A83 full 32x32 exclusive bounds |
| `128..159` | vertex shader | exact five instructions, control `60800004` |
| `160..195` | fragment shader | controls beginning `90018000`; TAU `301101b0`; REPLACE ALU `01c00000 301c0000 00f20105 000d083f` |
| `196..231` | immediate fan | TL, TR, BR, BL; `z=0,w=1`; S/T corners 0/1 |

The Windows UMD independently confirms the H6 topology and viewport/scissor
builders: D3D topology dispatcher `0x101f7410`, triangle fan builder
`0x101f5aa0`, `SetViewport` `0x100c99f0`, and `SetScissorRect` `0x100c9c00`.
The OpenGL topology dispatcher and fan builder are at `0x1024be90` and
`0x1024a520`.  No additional “enable emission” word appears in these
independently recovered closed builders beyond the FVF, VS, primitive count,
and raster state listed above.

The current kernel packet uses the same destination, sampler, FVF, fixed
viewport, clip, VS and REPLACE PS families, but emits a triangle list rather
than the vendor four-vertex fan.  That path is already live-proven by the
bounded texture smoke tests.  It cannot explain a trace containing no textured
ioctl.

## Exact proprietary zero-emission precedent

The proprietary Linux Xorg quad emitter contains one independently proven way
to accept texture state yet emit no primitive.  In `via_drv.so`,
`via3DEmitQuad_H5:0x2e162` tests the stage-zero transform pointer at state
offset `+0xd8`.  When non-null, it calls
`exa_get_transformed_coordinates()` for all four corners at:

```text
0x2e1b2, 0x2e20d, 0x2e268, 0x2e2bd
```

Any zero return branches directly to the function epilogue at `0x2ea9f`.
The type-00 immediate header is not written until `0x2e7b8`, so this is a true
zero-primitive exit after earlier destination/texture/shader state may already
have been emitted by the caller.  Valid coordinates are normalized by the
effective texture width and height at `0x2e6a8..0x2e727` before the four
vertices are written.

This gate belongs to the old EXA Picture-transform path; modern glamor
CopyArea performs its transform in the vertex shader and does not call this
binary.  Its importance is architectural: state or sampler activity is not
evidence that a primitive packet follows.  The modern equivalents are Xorg's
empty-intersection/FBO gates and draw's software clip/cull stages.

## Current live localization

`Xorg-noemit-v3.stdout` records exactly:

```text
triangle 0..5: kind=color+blend
sampler views set: shader=4 start=0 count=1 ... target=2 format=281 levels=0..0 resulting=1
```

There is no `kind=textured` line and no bounded-state rejection line.  The
debug line is printed in `chrome9_emit_triangle()` only after state validation,
vertex conversion, the nondegenerate test, and the draw-limit test
(`chrome9_draw.c:723-747`).  Therefore the trace proves that no textured
triangle reached the ioctl builder; it does not prove which earlier layer
dropped it.

The pixel-provenance run independently rules out an empty source as the reason
for a black copy.  Its alternating 300x300 imported sources have stride 1216,
about 58,100 RGB-nonzero pixels, and frame-varying hashes.  This makes sampler
contents a solved input for the no-emission discriminator, not a reason to
change A18/A20/A30 or swizzle state.

The remaining no-submit sites are:

| layer | exact no-emission condition | expected diagnostic today |
|---|---|---|
| Xorg CopyArea | empty computed intersection | sampler may be logged; no `draw_vbo` |
| Xorg destination | `glamor_pixmap_fbo_at()` returns null | sampler may be logged; operation bails |
| Gallium entry | zero draw count or zero instances | silent return at `chrome9_context.c:1494-1495` |
| draw pipeline | all geometry clipped, culled, or rasterizer-discarded | no vbuf triangle and no Chrome9 rejection |
| vbuf array bounds | range outside staged vertex count | silent return at `chrome9_draw.c:977-978` |
| vbuf validation | invalid layout/resource/state/finite values | increments rejected count; first state failure normally prints |
| fixed-point conversion | both output triangles collapse to zero area | silent return at `chrome9_draw.c:723-726` |
| draw limiter | `CHROME9_DRAW_LIMIT` reached | one explicit stop line |

For the intended glamor state, cull, depth, and rasterizer discard are off;
both quad triangles have consistent nonzero winding.  A stale software cull
state could still drop both before native emission, but changing Chrome9 A01
would not affect that stage.  Similarly, a malformed VS matrix could put all
vertices outside clip space or make viewport conversion collapse them even
though the fragment shader and sampler are valid.

## Minimal bounded discriminator

Instrument one 32x32 or 300x300 CopyArea without changing any packet word.
Preserve one record at each boundary:

1. In `glamor_copy.c`, log source/destination tile indices, `nbox`, `dx/dy`,
   source tile bounds, union bounds, the computed exclusive scissor, and
   whether the destination FBO exists immediately before line 479.
2. At `chrome9_draw_vbo()`, log primitive mode, count, instance count,
   framebuffer dimensions, viewport scale/translate, rasterizer
   `cull_face/front_ccw/scissor/rasterizer_discard`, and the scissor box.
3. At vbuf `set_primitive`, `draw_arrays`, and `draw_elements`, log primitive,
   staged vertex count, requested range/index count, and whether the software
   pipeline delivered any primitive.
4. Immediately before `chrome9_triangle_nondegenerate()`, log each final
   `(x,y,z,w,s,t)` and the signed fixed-point area.  Count degenerate returns
   explicitly for this run.

The required successful sequence is:

```text
source bind
  -> nonempty [left,right) x [top,bottom)
  -> valid destination FBO and viewport
  -> draw_vbo with four vertices
  -> vbuf quad/fan delivery
  -> two nondegenerate triangles with w=1
  -> two textured triangle submissions (or future batch entries)
  -> destination RGB hash changes inside the scissor only
```

For an identity 32x32 copy, the expected software outputs are the destination
rectangle corners and normalized S/T corners `(0,0)`, `(0,1)`, `(1,1)`,
`(1,0)`.  The existing kernel builder should then emit the established
triangle-list form.  If the sequence stops before vbuf, fix Xorg/FBO/state
plumbing.  If it reaches vbuf but the signed areas are zero, fix the
VS/viewport path.  Only a successfully submitted textured ioctl with wrong
pixels justifies changing native sampler, shader, blend, or format words.
