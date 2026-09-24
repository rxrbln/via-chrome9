# Chrome9 glmark2 clear current boundary

Status: exact installed glmark2 2023.01 source and current Chrome9 Gallium
clear-path audit, 2026-09-24.  This was an offline audit; no target GPU or
production code was used.

## Exact benchmark operation

The installed `clear` scene has no shader, geometry, resource, or state code.
`SceneClear` only calls the base `Scene` constructor and does not override
`setup()`, `draw()`, or `validate()`.  Its source is:

```text
scene-clear.cpp SHA-256
a7f0861a89cd31c75015ed186c1c5a13a2d0e24a3b635c778b051c94b14d518a
```

The benchmark work comes entirely from `MainLoop::draw()`:

```text
canvas_.clear();
scene_->draw();       // base Scene::draw(), empty
scene_->update();
canvas_.update();
```

For the desktop GL build, `CanvasGeneric::clear()` issues exactly:

```c
glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
glClearDepth(1.0f);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
```

The audited `canvas-generic.cpp` and `main-loop.cpp` SHA-256 values are
`3fc949c9eef6ee384a5b5a561479c32f8f380155b69511fb7ac74948c8be6c25`
and `cd11f7971bcb4dcc31c1c6a726bd8441d6471c4ff78d2412c3b5ffa57efa5106`.
There is no 3D draw and therefore no vertex shader, fragment shader, sampler,
varying, blend, or native instruction gate in this scene.

## Current Chrome9 route

The state tracker sends the normal full-buffer clear to
`chrome9_context.c:chrome9_clear()` with COLOR0 and DEPTH bits, full RGBA
mask, no stencil, and no scissor in the isolated default benchmark.  The
current implementation:

1. flushes a retained Chrome9 draw batch before modifying its targets;
2. accepts the exact COLOR0+DEPTH bit set and full color mask;
3. bounds the rectangle to the framebuffer dimensions;
4. packs and fills BGRX8888, BGRA8888, or RGB565 color storage on the CPU;
5. packs depth `1.0` as `0xffff` for Z16 or `0xffffff00` for X8Z24 and fills
   every depth pixel; and
6. immediately publishes a guarded render-target shadow back to an imported
   drawable resource.

The color is black in either BGRX or BGRA storage; alpha/X is one where the
format defines it.  The existing `chrome9-format-test` passes and explicitly
checks the Z16 and X8Z24 depth-one encodings.  Numerous screen-smoke paths
also use the same combined COLOR0+DEPTH clear before their guarded draws.

Consequently the isolated default clear scene is already inside the current
generic implementation.  It exposes no missing GPU feature and is not a
reason to add a clear packet or shader special case.  Its current path is a
synchronous CPU fill plus, for imported drawables, a CPU shadow publication;
that is a performance characteristic, not a correctness blocker.

## Why `Validation: Unknown` is expected

`SceneClear` inherits `Scene::validate()`, whose result is unconditionally
`ValidationUnknown`.  `MainLoopValidation` prints that value as
`Validation: Unknown`.  Thus the recorded clear result in
`glmark2-current-20260923.md` is not an image failure and can never become
`Validation: Success` without changing glmark2 itself.

The appropriate correctness oracle is an external readback, not glmark2's
status string.  Run one isolated frame with the ordinary default framebuffer,
read at least the four corners and center after `canvas_.update()`, and require
RGBA `(0,0,0,255)`.  A driver-level companion should prefill color and depth
with non-clear sentinels, invoke full COLOR0+DEPTH clear, then check every
logical row, row padding, both resource guards, Z16=`0xffff`, and shadow/public
drawable equality.  No command-22 submission should be attributed to the
scene.

Masked color, stencil, and application-scissored clears remain independent
driver features.  The current Chrome9 path rejects masked color/stencil
combinations, but the isolated default `clear` benchmark requests neither and
therefore does not qualify or block them.
