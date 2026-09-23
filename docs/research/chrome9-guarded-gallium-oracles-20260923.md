# Guarded Gallium D24 and A8 oracles, 2026-09-23

Target: VX900 Chrome9 HD at `192.168.2.60`, Linux `7.2.5-t2`,
`/dev/dri/renderD128`.  Chrome9 was loaded throughout.  The staged
`chrome9-argb4444.ko` SHA-256 was
`a56142363fc4a7a063321f0a65544793e1f55e7dd748d303dd6acff493ab5497`.
These are guarded resource tests, not evidence that all GL depth-texture or
glmark2 paths work.

## X8Z24 combined and depth-only Gallium draws

With `CHROME9_EXPERIMENTAL_X8Z24=1`, the Mesa screen smoke creates a private
guarded X8Z24 resource and a guarded BGRX color target.  One generic fragment
`MOV oC0,c0` triangle with D24 `LESS` wrote exactly 1,176 red BGRX pixels
(`0x00ff0000`), and the center D24 word was `0x80000000`.  Color and depth
changed on the same pixels; the full guard pages remained intact.  The first
run exposed a Mesa uniform-upload error: the compiler referenced c0 from a
NIR UBO but the submit path copied no buffer because its secondary uniform
mask was empty.  Mesa commit `1b2677b` selects externally bound constants
from the actual semantic constant mask excluding compiler immediates.  The
test's initial `0xffff0000` expectation was also wrong for BGRX storage;
the existing BGRX paths and this hardware store use `0x00ff0000`.

The same context then rebound a depth-only FBO, cleared D24 to `0xffffff00`,
and submitted the triangle again.  It changed exactly 1,176 depth pixels to
approximately 0.5 while the private scratch BGRX target remained byte-for-byte
`0xa5`, including all exterior pixels.  Color/depth/scratch guards survived;
each phase incremented the submitted triangle and program-batch counters by
exactly one without rejection.  Mesa commit `be3e6ac` adds this second phase.
The command was:

```sh
CHROME9_EXPERIMENTAL_X8Z24=1 chrome9-screen-smoke /dev/dri/renderD128 --d24-only
```

The experiment does not yet include Gallium sampling of that rendered D24
resource or an API-level depth-texture FBO; production exposure remains gated.

## A8 sampler

The guarded cmd22 sampler oracle's A8 mode passed eight distinct alpha
values.  Native `A30=0x301b0000` yielded B=G=R=0 and the original 8-bit alpha
in BGRA output for every sample.  The source BO and both guard pages were
unchanged.  The earlier X8Z24 raw sampler mode also passed after adding A8
mode.  Tool commit `f9d70b5` adds the test:

```sh
chrome9-sampler-guard-smoke /dev/dri/renderD128 --a8
```

This proves nearest A8 decode for these coordinates, not repeat/linear,
Gallium sampler-view selection, shader discard, or the glmark2 `ideas` scene.
