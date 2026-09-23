# glmark2 shadow: depth image preflight and next shader boundary

The `shadow` scene creates an unsized `GL_DEPTH_COMPONENT` / `GL_UNSIGNED_INT`
2D texture, changes its minification filter to `GL_NEAREST`, and attaches level
zero to a depth-only FBO. The Chrome9 format choice is `PIPE_FORMAT_X8Z24_UNORM`
when `CHROME9_EXPERIMENTAL_X8Z24=1`.

Before the Mesa state-tracker fix, the target log showed
`can-create ... X8Z24 ... last=9 ... bind=0`, followed by
`GL_OUT_OF_MEMORY in glTexImage2D(image too large)` and FBO status `0x8cd6`.
This was not an actual 640x480 mipmapped allocation. Real `glTexImage2D`
calls `st_TestProxyTexImage` with a proxy target; the proxy object's default
mipmap filter caused a full-chain estimate. `allocate_full_mipmap` for an
ordinary mutable depth image instead selects level zero. Mesa's proxy
preflight now uses `last_level=0` for mutable level-zero formats containing a
depth component, while immutable `numLevels>0` probes retain their requested
level count. Chrome9's actual multilevel X8Z24 rejection remains in place.

The rebuilt library was tested on the VX900 target with one bounded
`glmark2 --validate --off-screen --frame-end=readpixels -b shadow:nframes=1`
run. The final log at
`/root/tmp/chrome9-live/depth-proxy/shadow-final.log` shows the proxy
`can-create` query and actual resource creation both at `last=0`, followed by
a depth sampler view and no TexImage or FBO error. The scene is not rendered
correctly yet: the draw is rejected as an unsupported bounded fragment
program, and glmark reports `Validation: Unknown` for this one-frame variant.
The target's X server remained running and no new GPU fault appeared in dmesg.

The unsupported fragment NIR is captured at
`/root/tmp/chrome9-live/depth-proxy/shadow-nir.log`. It computes a projected
coordinate (`frcp` of input W), samples the depth texture, tests both positive
W and `texture.x < projected_z + 0.1505`, combines those booleans with `iand`,
and selects RGB `(0.15, 0.15, 0.15)` or `(0.30, 0.30, 0.30)`. This is the next
compiler/ISA task; FBO completeness alone is not a shadow-scene pass.

Keep actual multilevel X8Z24 fenced off until its rendering and sampling ABI
is implemented: the fragment kernel currently requires the depth BO size to
match the level-zero allocation exactly, and Mesa rejects depth mip sampling.
