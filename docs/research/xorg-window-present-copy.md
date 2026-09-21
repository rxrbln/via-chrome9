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
