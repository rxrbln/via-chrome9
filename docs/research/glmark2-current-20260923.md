# Current-driver glmark2 validation, 2026-09-23

Target: VX900 `1106:7122`, kernel `7.2.5-t2`, Chrome9 module SHA-256
`9f13173c7e6833c47fbc7dadb2016247b5b89c768c1db9290ae5f1c64f8cf6e3`.
Xorg's `/proc/<pid>/maps` resolved Gallium from the isolated staging path
`/root/tmp/chrome9-live/glmark-current-20260923/libgallium-26.2.2.so`, SHA-256
`a682753ae07c4c4e25713226dcd7da4225febd23e260f82a6d57c90574ad5f15`.
Xorg logged glamor OpenGL 2.1 and AIGLX Chrome9 initialization.  The GL 2.1
override is still a lab requirement and does not imply complete GL 2.1 support.

With that same DRI library in Xorg and the client, glmark2 2023.01 reported:

| Scene invocation | Result | Relevant evidence |
|---|---|---|
| `--validate -b buffer:nframes=1:update-fraction=10:update-method=map` | Validation: Success | Native fragment program 11 ALU, 0 TAU; `semantic=1 native=1 supported=1`; no kernel timeout |
| `--validate -b build` | Validation: Success | Default build scene; no kernel timeout |
| `--validate -b clear` | Validation: Unknown | No reference verdict; not counted as a pass |

The earlier `glmark-v6-buffer-compile.log` printed an old, abbreviated shader
debug format and predates even the staged Gallium library it was labelled
against.  It is not evidence that the current compiler lacks this arithmetic.
The current driver's no-draw 12-prefix replay also accepted the entire captured
VAR0, MUL, MIN, EXP2, MUL/ADD fragment expression before the live buffer run.

This record proves only the listed scene variants on the stated build.  It does
not validate the complete glmark2 set, all buffer variants, image correctness
beyond glmark2's reference verdict, or the packed float3 ALU/TAU path.

## Regression on the corrected shared-coordinate kernel

Later on 2026-09-23, the same Gallium library (SHA-256
`a682753ae07c4c4e25713226dcd7da4225febd23e260f82a6d57c90574ad5f15`)
was loaded by Xorg PID 554 with the A90-corrected kernel module (SHA-256
`e5b666b691b57fb275024bb7c8cdebf03e6c269c4bfc520be800330e02a37ca0`).
Under this one persistent Xorg session, a bounded
`glmark2 --validate` run reported `Validation: Success` for one frame each of
`texture:texture-filter=nearest`, `shading:shading=gouraud`, and
`buffer:update-fraction=10:update-method=map`.  glmark2 ignored the requested
300x300 size during validation and used its 800x600 reference size.  Xorg
remained alive and the Chrome9 kernel log contained no timeout, quarantine,
reset, or engine-error report.  This is a regression check on the corrected
module, not evidence that additional glmark2 scenes or arbitrary packed
varying shaders now pass.

## 2026-09-24 literal-packing Mesa build

The later client library at
`/root/tmp/chrome9-live/literal-pack/libgallium-26.2.2.so` has SHA-256
`34b195977f428bf76e01b61f17c12cd0ede36f02f3458f0279ef5ffe4f152159`.
The client loaded it from this path under the still-running X session.
Bounded `nframes=2` runs at 640x480 exited normally for the default
`effect2d` scene (38 FPS) and the four-window separable desktop blur scene
(14 FPS). The one-window blur variant completed at 22--23 FPS and produced a
recognizable captured frame; details and limits are in
`chrome9-glmark-desktop-blur-closure.md`. No new Chrome9 kernel fault appeared
in the bounded log checks.

At glmark2's fixed 800x600 validation size, default `effect2d` and desktop
blur reported `Validation: Unknown`; these are execution results, not pixel
proofs. The previously validated effect2d Laplacian kernel
`0,1,0;1,-4,1;0,1,0;` still reported `Validation: Success` with the new
library. Logs remain on the target under
`/root/tmp/chrome9-live/literal-pack/`.

## 2026-09-24 vertex-uniform capability correction

The next isolated client library, SHA-256
`1bfe606d2447974fb36229e1c2c32ee5a8ba82e96ec1c1f9f6d4819ce9d4a895`,
fixes a Mesa vertex-stage integer-capability mismatch which previously made
the default uniform-controlled `loop` vertex shader execute an effectively
unbounded number of iterations. The detailed diagnosis, six-case 0/1/2
uniform-versus-literal controls, and correction are recorded in
`chrome9-glmark-loop-vs-uniform-stall.md`.

With this library on the unchanged X/KMS session, glmark2 reported
`Validation: Success` for all three five-step loop fragment forms:
text-expanded, literal-bound, and uniform-bound. The earlier validated
`texture:texture-filter=nearest`, `shading:shading=gouraud`, and
`buffer:update-fraction=10:update-method=map` scenes also still reported
`Validation: Success`. The two-pass blur/FBO CPU-reference oracle again
passed both passes with a maximum RGB delta of one 8-bit level. Xorg remained
alive and the bounded kernel fault check found no new Chrome9 error. Logs
are retained under `/root/tmp/chrome9-live/vs-uniform-float/`.

The five-step `function` scene with low vertex/fragment complexity also
reported `Validation: Success` on that build. The corresponding
medium-complexity variant exited without a kernel fault but reported
`Validation: Failure`; Chrome9 logged `draw rejected: unsupported bounded
fragment program`. It remains an explicit compiler-feature gap, not a
passed scene. The target logs are `function-low-five-validate.log` and
`function-medium-five-validate.log` in the same staging directory.

## 2026-09-24 medium-function hardware validation

The new Mesa library (SHA-256
`a6d61f4f4666934a6eaec603eadf919867637e8ff667e5f223e3db79890c4136`)
enables standard NIR `fsqrt` lowering in the Chrome9 production compiler. It
was staged at `/root/tmp/chrome9-live/function-sqrt-20260924/` with the
scalar-selector-corrected kernel module (SHA-256
`f7e0c4023d813182842b7142e1943c2e01e384003abe6230b0c788e2bf83fcfb`).
Xorg PID 995 mapped that exact Gallium library, initialized glamor with the
lab-only GL 2.1 override, and initialized AIGLX through the previously built
Chrome9 DRI shim at `/root/tmp/chrome9-live/dri/chrome9_dri.so`.

Under that persistent X session, one frame of
`glmark2 --validate -b texture:nframes=1:texture-filter=nearest` reported
`Validation: Success`. The previously failing exact medium scene,
`glmark2 --validate -b function:fragment-complexity=medium:fragment-steps=5:vertex-complexity=medium:vertex-steps=5:nframes=1`,
then reported `Validation: Success` at 800x600. A second texture-nearest
validation also succeeded afterward. The bounded Chrome9 dmesg check found no
timeout, quarantine, engine error, fault, or draw rejection, and Xorg stayed
alive. Logs are `texture-nearest-validate.log`,
`function-medium-validate.log`, and `texture-after-function.log` in that
staging directory.

The complete glmark2 scene set and larger function variants remain unverified;
this result closes only the exact five-step medium function regression.
