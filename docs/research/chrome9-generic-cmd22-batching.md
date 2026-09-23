# Generic cmd22 batching: first live performance rung

On the 192.168.2.60 VX900 target, the current generic fragment path used one
`DRM_CHROME9_DRAW_FRAGMENT_PROGRAM` ioctl (command `0x1d`, DRM ioctl `0x5d`)
per triangle.  The glmark2 `build:model=cat` frame made 8,043 such calls;
`strace -c` measured about 5.44 seconds in ioctl alone.  The Mesa path set
`triangle_count = 1` even though the kernel UAPI accepts up to 128.

Mesa now collects up to 16 triangles with an identical immutable cmd22 state
key: destination/depth, depth/blend/scissor, instructions, constants, and all
texture descriptors.  It copies each vertex into owned storage, flushes at a
state mismatch or capacity, and flushes before returning from each Gallium
`draw_arrays`/`draw_elements` callback so BO handles cannot outlive bindings.
The existing framebuffer, clear, and explicit flush paths retain ordering.
Submission, success/failure counters, and render-target dirty tracking happen
at batch flush.  A queued flag avoids counting a failed flush twice.

| Rung | Cat cmd22 calls/frame | Traced cat frame | Notes |
|---|---:|---:|---|
| Old, one triangle | 8,043 | about 6.2 s | Baseline trace |
| Two triangles | 4,022 | 3.348 s | No kernel GPU fault |
| Sixteen triangles | 503 | 0.529 s | No kernel GPU fault |

The untraced 320x240, two-frame cat run improved from about 4.49 seconds to
0.466 seconds/frame; Gouraud cat from 4.48 seconds to 0.470 seconds/frame.
Cube build and nearest-texture cube still complete at 348 and 277 FPS in the
same short run.  glmark2 validation reported success for build (VBO and
non-VBO), nearest/linear/mipmap texture, and Gouraud shading.  These are
glmark2's own validation results, not a full image conformance proof.
One later single-frame multi-scene run reported `EAGAIN` on its first Gouraud
draw; isolated Gouraud and a repeated two-frame cat/Gouraud sequence both
completed without that error.  Its cause is unresolved and needs a targeted
trace before any claim of fully reliable execution.

`effect2d` remains invalid in both the old one-triangle Mesa and this batch
branch: an isolated kernel `0,1,0;1,-4,1;0,1,0;` draw returns `EINVAL` on
cmd22.  Other glmark2 validation failures still include unsupported fragment
programs, vertex texture fetch, and depth render targets.  Do not attribute
those to batching without a matched baseline.

The cap remains 16, not the UAPI maximum 128.  The current kernel builder
checks stage length only after writing its packet.  A conservative 16-triangle
maximum is about 12 KiB within the 64-KiB stage; 128 can approach that limit
and requires a pre-write bound before silicon testing.  This is a bounded
performance change, not evidence that the whole GL feature set is complete.
