# Xorg window Present copy and direct-flip discriminator

This note separates two presentation paths which must not be conflated on the
single Chrome9 device:

```text
windowed GLX
  Chrome9 application back pixmap
    -> X Present COPY / glamor texture draw
      -> Chrome9 root scanout pixmap (same active FB/base)

eligible fullscreen Present
  Chrome9 application pixmap
    -> atomic KMS FB change
      -> IGA1 direct-SF 0214 base update
```

There is no PRIME/offload or second GPU in either path.  DRI3 is only the
same-device sharing transport.  A 300x300 window on the 1024x768 root cannot
by itself be scanned out, so an all-black `xwd -name glxgears` is primarily a
COPY-path observation, not evidence that a KMS base flip failed.

## Measured failure split

Mesa `86cb963` renders the same fixed-function gears workload correctly in a
surfaceless framebuffer: three frames, three gears, D16 enabled,
39325/65536 changed pixels, checksum `be5ea2b39deb130c`, and `040c=0`.
Windowed `glxgears` under both default DRI3 and
`LIBGL_DRI3_DISABLE=1` ran at about 19--20 FPS, but its exact 300x300 XWD was
black.  The default-run root XWD was also black.

The XWD payloads provide a stronger check than their file hashes.  After the
107/109-byte headers and 256 12-byte color records:

```text
1024x768 root payload:     3145728 zero bytes
300x300 window payload:    360000 bytes, exactly 90000 nonzero bytes
```

The one nonzero byte per 32-bpp window pixel is the opaque/padding byte; every
RGB byte is zero.  Thus this is not a stale screenshot containing an
undetected gear color.

The server log reaches glamor, DRI2, DRI3, Present and GLX successfully.  Its
one Chrome9 draw diagnostic is:

```text
reason=texture-count/depth fs=7 layout=1 tex=1 views=0 samplers=1
target=105 depth=0 ... blend_successor=1
```

`fs=7` is `CHROME9_FS_TEXTURE_REPLACE`.  The state has the expected one
texture sampler but no bound sampler view, so that particular triangle is
rejected before an ioctl.  The warning occurs during server startup and the
driver suppresses later draw-state warnings with one context-wide bit; it is
therefore a candidate for the black Present copy, not yet proof that every
swap fails for the same reason.

## Exact glamor COPY shader

The installed Xorg 21.1.24 `libglamoregl.so` carries its `copy_area` shader at
string offsets `0x22cfb..0x22de2`.  Its semantics are exactly the already
classified one-texture REPLACE form:

```glsl
/* vertex */
gl_Position.xy = primitive.xy * v_matrix.xz + v_matrix.yw;
gl_Position.zw = vec2(0.0, 1.0);
fill_pos = (fill_offset + primitive.xy) * fill_size_inv;

/* fragment */
frag_color = texture(sampler, fill_pos);
```

The general composite shader instead starts around `0x2352f` and combines
source and mask.  A plain window swap should first be proven through
`copy_area`; extending general composite shaders is not a justified response
until the trace shows that Present selected them.

Mesa's DRI3 loader calls `xcb_present_pixmap()` for every window swap.  The X
server chooses flip or copy.  The loader does not directly issue a KMS flip
for a 300x300 window.

## Authoritative live discriminator

Load the polled direct-flip kernel from commit `bf2d4a3` with dynamic debug
enabled for Chrome9 KMS.  Its reviewed module has SHA-256
`b4ee5b1aff68a095adaaea3fe6d7a9618160bf5ba891211bf3df0c7b57d2127b`.
Do not infer a flip merely because Present reports swap completion.

For a bounded windowed run, collect together:

1. `chrome9_display_snapshot` before, during and after the run;
2. `/sys/kernel/debug/dri/0/state` and `framebuffer`;
3. kernel lines containing `direct SF flip latched` or
   `direct SF flip failed`;
4. a short `strace -f -yy -e trace=ioctl` attached only to the Xorg process.

Interpretation is exact:

| observation | conclusion |
|---|---|
| active FB id and `0214[28:0]` stay constant; no latch log | Present selected COPY; KMS flip code was not exercised |
| FB id changes; latch log has a new SF byte base; `0204.bit1=0`; masked `0214` matches | direct flip completed by the kernel predicate |
| `direct SF flip failed` | presentation is quarantined; both possibly fetched mappings remain pinned |
| Xorg trace has no Chrome9 draw ioctl while swaps continue | glamor rejected/fell back before hardware submission |
| Xorg trace issues command number `0x45` or `0x47` and it returns zero | one-texture COPY/one-texture blend COPY reached and retired in the kernel |
| the same ioctl returns an error | use its errno and the first Mesa rejection line; do not alter KMS |

Chrome9 command numbers `0x45` and `0x47` are respectively
`DRAW_TEXTURED_TRIANGLE` and `DRAW_TEXTURED_TRIANGLE_BLEND` after
`DRM_COMMAND_BASE`.  `strace` may print their numeric `_IOC` form rather than
their names.  Application rendering is in the glxgears process; attaching
only to Xorg makes a server-side COPY ioctl unambiguous.

If no Xorg draw ioctl occurs, repeat one bounded run with
`CHROME9_DUMP_NIR=1` and `CHROME9_DEBUG_RESOURCES=1` in the Xorg environment.
Require a non-null sampler view bind before the first copy triangle.  The
smallest suspected state failure is the measured `views=0`; do not broaden
the shader classifier because the emitted FS was already correctly classified
as REPLACE.

If an Xorg COPY ioctl succeeds while the window remains black, the next split
is source versus destination:

* map/read the application pixmap only after its producer has retired and
  prove nonblack RGB;
* capture the active root FB and require the 300x300 destination rectangle to
  change;
* keep the active FB id and `0214` fixed for this test;
* require `040c=0`, completed submit status, and no guard damage.

Nonblack source plus unchanged destination is a glamor coordinates,
blend/scissor, or destination-import problem.  A changed root BO with stale
physical output is instead the unchanged-front-buffer cache/device-ownership
bridge; the render-retirement `chrome9_sf_resync_object_locked()` path should
then be audited by exact GEM-object identity.

## Accepted draw but black external pixmap: localization gate

Mesa `067bf9f` fixes the sampler-view gate.  The live Xorg process now reports
one non-null BGRX view (`format=281`, `resulting=1`), the glamor texture draw is
accepted, and the bounded kernel submission retires without an engine error.
Nevertheless both the 300x300 window XWD and the complete root XWD remain
RGB-zero.  Render-target shadow lifetime was a plausible driver-local cause,
but the direct discriminator has now rejected it as the sole cause.

When `set_framebuffer_state()` receives an unguarded external color resource,
it creates a canonical guarded `ctx->render_target` and copies external pixels
into it once.  Successful draw submissions and clears set
`ctx->render_target_dirty`.  The reverse copy is performed only by:

```text
set_framebuffer_state() before replacing the binding
pipe_context::flush()
pipe_context::flush_resource()
context destruction
```

Keeping the same glamor framebuffer bound does not exercise the first path.
`chrome9_transfer_map()` ignores its pipe context and returns the external
BO mapping directly, so an XWD/readback map does not publish the dirty shadow
either.  If this frontend path does not call `flush` or `flush_resource` at
the Present boundary, the guarded shadow contains the completed hardware
draw forever while the X pixmap and scanout object stay black.  That behavior
was consistent with the symptom but not unique to it.  A direct guarded target
does not need a byte copy, but uses the same dirty flag.

The discriminator added immediate publication at the outer Gallium-operation
boundary, not in the per-triangle callback:

```c
draw_vbo(...);
draw_flush(ctx->draw);               /* all Chrome9 ioctls are synchronous */
chrome9_flush_render_target(ctx);    /* one shadow copy per Gallium draw */
```

Placing the call after `draw_flush()` includes every internal triangle while
avoiding a full-surface copy per triangle or ioctl batch.  The test also
published immediately after `chrome9_clear()` and
`chrome9_clear_render_target()`.  The complete Chrome9 smoke suite passed, but
the live root and named-window XWD RGB payloads remained byte-for-byte
identical and all zero.  Therefore delayed target-shadow copyback is not the
remaining black-window cause.  Keep the publication fix for correct resource
semantics, but do not use it as evidence that the sampled source or Present
destination ever contained rendered pixels.

A `transfer_map` flush is useful but not sufficient for Present: it guarantees
CPU readback only when a map happens and does not make the X pixmap visible to
display/composite consumers which never map it.  For complete map semantics,
the later implementation should also:

* call `chrome9_flush_render_target()` before mapping the currently bound
  external color resource, so reads and partial writes start from current
  pixels; and
* after unmapping that resource with a write usage, copy external back into
  the still-bound guarded shadow, otherwise a later hardware draw can restore
  stale pre-map pixels in untouched regions.

Keep the existing `flush`, `flush_resource`, rebind, and destroy publication
points as redundant ordering boundaries.  The next run must checksum each
side of the cross-process Present chain rather than infer contents from a
successful ioctl.

### One-run pixel provenance instrumentation

Install the same opt-in diagnostic in both the GL client and Xorg Chrome9
processes.  Every record needs PID, context pointer, resource pointer, BO
handle and size, format, width/height, stride, data offset, guarded flag, a
64-bit hash of useful rows, RGB-nonzero pixel count, and the coordinate/value
of the first RGB-nonzero pixel.  A count of arbitrary nonzero bytes is not a
color test: the measured black XR24 window already has one nonzero X/alpha
byte per pixel.  Handles are local to each DRM file, so geometry, layout and
hash -- not numerical handle equality -- correlate the client export with the
Xorg import.

Record these ordered stages after the synchronous producer has retired:

| stage | resource to checksum |
|---|---|
| client `draw-target-after` | effective guarded render target immediately after `draw_flush()` |
| client `render-external-after` | public/exported resource immediately after shadow publication |
| Xorg `sampler-external` | `view->texture` before any source-shadow refresh |
| Xorg `sampler-effective` | refreshed shadow when one exists, otherwise the direct `view->texture` |
| Xorg `draw-target-before` | effective destination immediately before the accepted glamor draw |
| Xorg `draw-target-after` | effective destination immediately after `draw_flush()` |
| Xorg `target-external-after` | public destination immediately after target publication |

The direct sampler case is essential.  A canonical guarded same-device import
has no `cview->shadow`; instrumentation placed only inside
`if (cview->shadow)` silently omits exactly the likely DRI3 source.  Always
report `view->texture`, then report either the refreshed shadow or an explicit
`sampler-direct` record.

Do not use one small global line budget: repeated client clear/draw operations
can exhaust it before Xorg reaches the relevant copy.  Bound output per
PID/stage/resource, preserving at least the first zero observation and the
first RGB-nonzero transition.  This provides a deterministic split:

| first failed transition | conclusion |
|---|---|
| client draw target is RGB-zero | failure remains in client rendering/state |
| client target is nonzero, exported external stays zero | client target publication failure |
| client external is nonzero, Xorg sampler external is zero | stale/wrong DRI3 pixmap, handle lifetime, or producer-fence ordering |
| sampler external is nonzero, effective sampler is zero | sampler shadow/layout refresh failure |
| effective sampler is nonzero, Xorg draw target remains zero | glamor geometry, viewport/scissor, blend, or destination state failure |
| Xorg draw target is nonzero, public destination stays zero | Xorg target publication failure |
| public destination is nonzero, named XWD is zero | the draw targeted a different/offscreen window pixmap |
| 300x300 destination becomes nonzero but 1024x768 root stays zero | missing later window-to-root composite/damage path |

The final two cases distinguish an offscreen redirected window pixmap from the
root scanout pixmap without guessing.  Width and height in every record are
therefore part of the correctness evidence, not merely diagnostic decoration.

### Decisive one-page DRI3 offset oracle

A 300x300 client back buffer filled with three 100-pixel vertical bands gives
an address-level discriminator which excludes raster, viewport and texture
interpolation state.  Its canonical pitch is 1216 bytes, or 304 physical
pixels: red `[0,100)`, green `[100,200)`, blue `[200,300)`, then four black
padding pixels.  The Xorg Present copy at root `(100,40)` produced these
transitions within the window:

```text
x=0 green, x=8 blue, x=108 black, x=112 red, x=213 green
```

That is exactly the physical source row read from texel 192:
green 8, blue 100, padding 4, red 100/101, green 87.  The displacement is not
an inferred coordinate error:

```text
4096 mod 1216 = 448 bytes = 112 texels
304 - 112 = 192
```

In particular, a viewport or interpolator error cannot insert the BO's four
physical row-padding pixels into the sampled image.  Explicit type-02
A04=`04000000`, absolute destination XY `(100,40)..(400,340)`, normalized ST
`0..1`, and REPEAT addressing do not change this conclusion.

CPU mapping before the first Xorg Chrome9 submit completed the proof.  The
client's `GL_BACK` capture still had the intended bands, while Xorg imported
the BO as stride 1216, data offset 4096, size 376832.  `map+4096` already held
the rotated row, and the nominal first pixel held the guard cookie
`13579bdf`.  Therefore the cross-process image exchange discarded the plane
offset at the producer side even though the consumer reconstructed a
canonical guarded `+4096` tuple.  No 3D packet change can repair this.

The bounded compatibility contract is:

* allocate every level-zero `PIPE_BIND_SHARED` XRGB/BGRA resource with pixels
  at BO byte zero, canonical 32-byte pitch, page-rounded size, and
  `guarded=false`;
* export KMS and dma-buf handles with offset zero and linear modifier;
* classify an offset-zero import as unguarded even when its size happens to
  equal a guarded allocation;
* require the existing guarded render-target shadow plus flush/copyback for
  every hardware render into that public object;
* require the existing per-draw refreshed guarded sampler shadow for every
  hardware sample from it; and
* allow KMS scanout to consume offset zero directly, which is already an
  explicit accepted layout.  Do not pass an unguarded shared BO to any 3D
  ioctl: the kernel draw ABI intentionally continues to add one page before
  target/texture pixels.

Private and mipmapped resources retain the exact canonical leading-page,
level-zero, trailing-guard layout.  Shared mipmapped allocations remain
rejected until cross-process per-level layout metadata exists.  The focused
regression must cover an offset-zero XRGB shared export, re-import with Xorg's
render-target/sampler bindings but without `PIPE_BIND_SHARED`, guarded
render-target execution through the shadow, BGRA GBM-style allocation, and
the existing draw-refreshed sampler-shadow stale-update oracle.

This follows the exact Xorg 21.1.24 call chain.  `present_execute_copy()` calls
`present_copy_region(&window->drawable, source, ...)`, whose scratch GC invokes
`CopyArea(source, window drawable, ...)` and only then calls the screen flush.
Glamor's FBO copy resolves both drawables with
`glamor_get_drawable_pixmap()`; a window resolves through
`Screen::GetWindowPixmap(window)`.  It does not unconditionally select the
screen pixmap.  Thus the effective destination is the 1024x768 root/front BO
for an ordinary unredirected window, but can be a 300x300 offscreen BO for a
redirected window.  The source references are Xorg 21.1.24
`present/present_execute.c:65-90`, `present/present.c:65-95`,
`glamor/glamor.c:57-75`, and `glamor/glamor_copy.c:358-475`.

## No-reboot containment and fallback

On a normal run, stop the GL client and Xorg and require
`restored pre-KMS IGA1 VGA state` before quiescing the engine.  Only after that
line and empty DRM framebuffer state is it safe to use `engine_quiesce=1` and
unload the module.  The BAR2 copy fallback remains explicit:

```text
fixed_iga1=1 staging_scanout=1
scanout_offset=0x6000000 scanout_size=0x1000000
```

It is a diagnostic fallback, not the final architecture.

After a polled-flip failure, the retained event can make atomic shutdown wait
for the generic DRM dependency timeout before `pipe_disable` runs.  Stop all
clients once and allow that bounded wait.  If the confirmed restore line then
appears, validate the saved VGA image and proceed with engine quiesce/unload.
If VGA acquisition/restore fails, `040c` becomes nonzero, the engine remains
newly busy, or a mapping-retained message remains, do not force unbind,
quiesce, unload, restart Xorg, or submit another GPU job.  Reboot is the only
safe recovery from that containment state.
