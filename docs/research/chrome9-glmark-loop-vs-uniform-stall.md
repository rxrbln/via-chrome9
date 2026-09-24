# Chrome9 glmark2 loop vertex-uniform stall (2026-09-24)

Status: root cause fixed in Chrome9 Gallium; three installed five-step loop
variants now report glmark2 `Validation: Success` on VX900.

On the 192.168.2.60 VX900 target with the literal-packing Mesa client library,
`glmark2 --validate --benchmark
"loop:fragment-loop=false:fragment-uniform=false:fragment-steps=5:nframes=1"`
did not finish within 20 seconds. The 320x240 variant also timed out after
six seconds. Both printed the scene start and no result. A syscall trace of
the 320x240 case ended in user-space work, without an in-flight Chrome9 draw
ioctl. A bounded GDB attach located the active stack at:

```text
micro_frc
exec_vector_unary
exec_instruction
tgsi_exec_machine_run
vs_exec_run_linear
fetch_pipeline_generic
draw_vbo
chrome9_draw_vbo
```

The default glmark2 loop options keep `vertex-loop=true` and
`vertex-uniform=true`, with `vertex-steps=1`. The vertex source iterates
`for (int i=0; i<VertexLoops; i++) d=fract(3.0*d)` and glmark2 uploads the
integer uniform with `glUniform1i`. The vertex grid has a default size of
32, so the timeout is not explained by an intentionally enormous mesh.

Two controlled variants isolate the failing dimension. With the same
five-step fragment shader, `vertex-loop=false:vertex-steps=1` completed at
320x240 in approximately 0.25 seconds. `vertex-loop=true` with literal
`vertex-uniform=false:vertex-steps=1` also completed. Keeping the uniform
loop but setting `vertex-steps=0` completed. Thus the nonzero uniform-bound
vertex loop in the draw-module interpreter is the suspect, not the tested
fragment shader or a wedged GPU. A known-good guarded fragment draw passed
after the timeout, Xorg PID 512 remained alive, and the bounded kernel log
check found no new Chrome9 fault.

The unmodified same-size, same-option glmark2 run with
`LIBGL_ALWAYS_SOFTWARE=1` completed on llvmpipe in about 0.4 seconds
(one-frame 31 FPS report). This rules out the application's loop being
intrinsically unbounded and further narrows the defect to Chrome9's
draw-module/NIR-to-TGSI path or its constant-buffer integration.

A non-rendering GDB breakpoint at `tgsi_exec_set_constant_buffers` exposed
CB0's 80-byte payload before vertex execution. Dwords 0--15 contain the
4x4 transform; dword 16, corresponding to `VertexLoops`, is
`0x3f800000` (float 1.0) even though glmark2 calls `glUniform1i(...,1)`.
If the NIR-to-TGSI interpreter consumes that dword as an integer, the loop
bound is 1,065,353,216 rather than one. This is a direct explanation for
the prolonged `micro_frc` stack and the zero-bound control passing, but the
representation contract must be traced before changing Mesa. The breakpoint
trace is `/root/tmp/chrome9-live/literal-pack/loop-const-gdb.txt`.

Target evidence is retained at
`/root/tmp/chrome9-live/literal-pack/loop-literal-validate.log`,
`loop-literal.strace`, `loop-literal-stack.txt`,
`loop-no-vs-loop.log`, `loop-literal-vs-loop.log`, and
`loop-zero-vs-uniform.log`, plus `loop-llvmpipe-control.log` and the GDB
constant-buffer trace. Do not count this glmark2 variant as passing.
No GPU packet change was indicated by this evidence.

## Root cause and correction

`draw_init_shader_caps(vs)` inherited `vs->integers=true` from the generic
TGSI interpreter, while Chrome9 fragment caps did not advertise integers.
For GLSL 1.20, Mesa therefore selected its shared non-native integer-uniform
representation and stored `VertexLoops=1` as float `1.0`. Conversely,
`nir_to_tgsi` used the per-vertex-stage integer capability to retain native
integer comparisons and loop increments, reading those bits as the integer
1,065,353,216. This exactly explained the measured long-running vertex loop.

Chrome9 now explicitly sets `vs->integers=false`, matching the shared Mesa
uniform-storage contract. The NIR-to-TGSI path then lowers vertex integer
arithmetic/comparisons to float operations. The screen smoke capability
contract asserts that neither Chrome9 shader stage advertises native integers.
This is a stage-capability correction, not a uniform-buffer byte rewrite or
an application-specific loop rule. Mesa Chrome9 commit `9f4d9a9` and parent
Mesa commit `e9e245c` contain the change.

The rebuilt client library (SHA-256
`1bfe606d2447974fb36229e1c2c32ee5a8ba82e96ec1c1f9f6d4819ce9d4a895`)
was staged separately at `/root/tmp/chrome9-live/vs-uniform-float/`.
Uniform and literal vertex bounds 0, 1, and 2 all completed at 320x240.
At glmark2's 800x600 validation size, all three installed five-step fragment
forms—text-expanded, literal-bound loop, and uniform-bound loop—reported
`Validation: Success` with a five-step uniform vertex loop. Xorg survived,
and the bounded kernel log checks found no new Chrome9 fault. The three logs
are `loop-text-five-validate.log`, `loop-literal-five-validate.log`, and
`loop-uniform-five-validate.log` under the staging directory.
