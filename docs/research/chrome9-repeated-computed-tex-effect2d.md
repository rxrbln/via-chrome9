# Repeated computed TEX on sampler 0 (VX900, 2026-09-24)

## Failure and fix

glmark2 `effect2d` initially failed the Chrome9 command-22 validator with
`EINVAL`: Mesa lowered every 2D TEX coordinate as `XYWW`, including a TEMP
whose preceding ALU instruction defined only XY. Mesa now uses `XY01` for a
computed TEMP while retaining the direct varying's native `XYWW` mapping.

That exposed a second, independent kernel gate: the fragment encoder rejected
two computed coordinates for sampler 0 with `EOPNOTSUPP`. A01 needs one
stage-zero coordinate for FE setup, but each computed TEX selects its own
coordinate slot in TAU. The encoder now retains the first computed slot for
FE setup, accepts subsequent computed slots, and still rejects conflicting
direct stage-zero varying coordinates. Kernel compile selftests cover both
cases and assert the expected TAU/ALU counts and A91/A92 controls.

## Hardware checks

The guarded `chrome9_vpos_texture_guard_smoke --repeat-two` program samples
the same texture using distinct `vPos/32` and `vPos/16` coordinates, multiplies
the fetched colors, and checks 16 pixels, D16, exterior pixels, buffer guards,
and source preservation. It passed on VX900 with the new module. The existing
single-computed-coordinate control also passed.

With that module and the new Mesa library, the glmark2 Laplacian scene
`effect2d:kernel=0,1,0;1,-4,1;0,1,0;:nframes=1` ran under X at 320x240.
Its command-22 ioctl trace contained no `EINVAL`, `EOPNOTSUPP`, `EIO`, or
`ETIMEDOUT`. At glmark2's fixed 800x600 validation size, the same scene
reported `Validation: Success`. The identity and all-ones kernels submitted,
but glmark2 reported `Validation: Unknown` for those configurations, so they
are not claimed as image-correctness proofs.

This establishes a two-fetch hardware oracle and the specific installed
Laplacian scene; it does not establish every repeated-fetch schedule or all
convolution kernels. The driver still reports an experimental OpenGL 2.1
renderer, not a complete feature set.
