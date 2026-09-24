# Chrome9 generic fragment primary color

The first generic cmd22 shader source for vertex COLOR0 is now separate from
T varyings.  The Windows shader compiler's `MOV oC0,v0` source word is
`00f04005` (native class 1, ID 0); `MOV oC0,t0` is `00f0c005` (class 3,
ID 0).  The older VIA Linux production compiler agrees.  See
`shader-isa.md` and `via-5.75-shader-re.md` for the token-level corpus.

UAPI `DRM_CHROME9_FRAGMENT_FILE_PRIMARY=4` accepts only index zero and is
advertised with capability bit 54.  The kernel already emits normalized
packed D1 for every cmd22 vertex.  Its compiler now encodes PRIMARY as
class 1/ID 0 without taking a T-coordinate slot or setting a T bit in A90.
The kernel self-test checks both the exact `MOV` record and the production
`TEX r0,t0,s0; MUL oC0,r0,v0` ALU/TAU/control tuple.

Mesa's semantic compiler maps COL0 input to PRIMARY and keeps its input mask
separate from the texture-varying mask.  Generic draws emit COLOR0 as one
`EMIT_4UB_BGRA` dword after position, then the T varyings; finite-float
validation skips the packed color word.  Capability gating prevents this
path from running on older kernels.  Input ID 1 / D2 specular is deferred.

Target `192.168.2.60`, 2026-09-24: the rebuilt module loaded and its
self-tests passed.  A single guarded raw `MOV oC0,PRIMARY0` draw wrote the
exact 1,176-pixel triangle in `ff4080c0`, with zero exterior and unchanged
guard pages.  X then initialized glamor on the separately staged Mesa build;
`glxinfo -B` reported accelerated direct OpenGL 2.1.  The raw oracle proves
the D1 shader encoding and channel order, but a focused Gallium
PRIMARY-plus-T layout draw is still required.

The subsequent 8-second `glxgears` sanity run rendered 479 frames in its
first 5-second sample (~95.6 FPS) but then reported an unrelated legacy
`DRAW_RHW_RUN_TRIANGLE_D16_RHW_BATCH` EIO after 178,276 triangles.  The
kernel logged `SF draw-cache bind cleanup failed; mapping retained for
reboot`.  No cmd22 PRIMARY rejection or packet fault was logged.  Treat the
GART mapping failure as an open issue; no further GPU commands should be
issued on that module instance.  The blend/MODULATE safety gate remains shut.
