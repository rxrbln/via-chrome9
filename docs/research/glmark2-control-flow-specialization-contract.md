# Chrome9 glmark2 control-flow specialization contract

Status: implementation contract backed by the installed glmark2 2023.01
sources, captured Chrome9 NIR, Mesa 26.2.2 source, and exact output from the
production VIA 5.75 pixel compiler.  This is a generic shader contract.  No
scene name, source hash, benchmark option, or application identity may be a
compiler key.

## Result

Chrome9 fragment control flow must be eliminated before native pixel-shader
selection.  The recovered PS 2.0 path is straight-line:

1. literal-bound loops are unrolled by NIR;
2. a loop whose bound is a uniform is compiled as a bounded variant after the
   exact value for that draw is substituted;
3. side-effect-free conditionals are if-converted to `bcsel` and selected with
   native CMP;
4. function calls are inlined by the GLSL/NIR linker;
5. discard is not a CMP and must use an independently proven kill/alpha-test
   path;
6. any loop, branch, call, or phi left after the late passes rejects the native
   variant.

There is no recovered native fragment predicate register or fragment branch
stack.  Production Windows compilation of `IF c0; MOV oC0,c1; ENDIF` returns
success but emits only the MOV.  The Linux PS 2.0 dispatcher has no CALL, IF,
or LOOP entry, and IFC/BREAK/BREAKC may parse without emitting a record.  Such
"success" is negative evidence, not a capability.

## Exact glmark2 forms

The installed source is
`/srv/t2/src.glmark2.x86-64-desktop.260917.193020.2191764/glmark2-2023.01`.
Its default benchmark list has three loop cases, all with five fragment steps:
text-expanded (`fragment-loop=false`), literal-bound
(`fragment-uniform=false`), and uniform-bound (`fragment-uniform=true`).

The generated uniform fragment loop is exactly:

```glsl
uniform int FragmentLoops;
float d = fract(gl_FragCoord.x * gl_FragCoord.y * 0.0001);
for (int i = 0; i < FragmentLoops; i++)
    d = fract(3.0 * d);
gl_FragColor = vec4(d, d, d, 1.0);
```

`SceneLoop::setup()` uploads `FragmentLoops = fragment-steps` once after
linking.  It is uniform for a draw and is not compile-time constant when
`fragment-uniform=true`.

The Chrome9 NIR captured in `/tmp/chrome9-glmark-nir-v4/nir/loop.err` makes
the exact incoming form unambiguous:

```text
load_uniform base=1 float32  // gl_FbWposYTransform
...
loop {
    d = phi(prefix, next_d)
    i = phi(0, i_next)
    count = load_uniform base=0 int32  // FragmentLoops
    stop = ige(i, count)
    if stop { break; }
    next_d = ffract(fmul(3.0, d))
    i_next = iadd(i, 1)
}
```

This also proves that blindly treating every uniform as part of a variant is
wrong: dword zero decides the loop; the framebuffer Y transform in the next
vec4 slot remains a normal runtime constant.

The other benchmark families are simpler than their names suggest:

- `conditionals` repeats a finite `if (d >= 0.5)` source fragment.  The
  captured one-step NIR is already straight-line `fge` plus two `ffract`
  arms and `bcsel`.  Five steps are five dependent select regions, not native
  branch nesting.
- `function` repeats `d = process(d)` in source.  Calls are inlined at link
  time; the captured NIR has no call.  Low complexity is MUL+FRC.  Medium adds
  multiply, square root lowering, and FRC, but no control flow.
- `ideas-lamp-lit.frag` has a literal loop bound of three.  Its `unitvec`
  returns have already become `feq`, `iand`, and nested `bcsel` in the captured
  NIR.  Only the outer fixed loop remains because Chrome9 currently disables
  NIR unrolling.
- `ideas-logo.frag` has the same branch-free `unitvec` select tree and no loop.
- `light-cel.frag` and `shadow.frag` contain data-dependent but side-effect-free
  color assignments.  They are legal if-conversion candidates.
- `terrain.frag` defines `MAX_POINT_LIGHTS` as one.  Its loop is literal-bound
  and its distance conditional is a select.  Shipped `USE_FOG` is commented
  out, so the default scene does not use the fog branch.
- `ideas-logo-shadow.frag` contains `terminate_if` after sampling alpha.  It is
  the one shipped fragment branch that cannot become an ordinary CMP because
  its side effect is fragment termination.
- time-dependent conditions in the ideas vertex shaders run in the Gallium
  draw module today.  They are not a native Chrome9 pixel-flow requirement.

The full source-to-action inventory is preserved in
[`glmark2-control-flow-matrix.tsv`](glmark2-control-flow-matrix.tsv).

## Why current Mesa refuses the loop

`chrome9_nir_options` currently sets arithmetic lowering flags but leaves
`max_unroll_iterations` zero.  `gl_nir_opts()` calls
`nir_opt_loop_unroll` only when that option is nonzero.  Consequently even the
literal bounds 1, 3, and 5 survive to `chrome9_create_fs_state()`, whose native
path first requires a straight-line entrypoint.

The current fragment state owns one NIR pointer and one native program.  It
compiles only in `create_fs_state`; there is no per-uniform variant cache.
`set_constant_buffer` only retains a buffer reference.  These facts explain
the rejection but do not justify adding native branch words.

The first safe link-time change is:

```c
.max_unroll_iterations = 32,
```

That closes all literal glmark bounds (one, three, and five) and leaves an
oversized or unprovable loop intact for deterministic rejection.  It does not
close the uniform-bound case.

## Reuse the proven Mesa specialization path

Do not invent a draw-time constant-buffer parser.  Mesa already has the
official value channel, and the sibling `m9x` driver in this source tree has a
complete working example:

- `m9_specialise.c` discovers constant-buffer dwords controlling an if or loop
  exit, substitutes only those dwords, and runs the late folding/unroll loop;
- `m9_screen.c:m9_finalize_nir` records them in
  `nir->info.inlinable_uniform_dw_offsets`;
- `call_finalize_nir_in_linker=true` makes that metadata reach `gl_program`;
- `st_atom_constbuf.c` sends the exact current values through
  `pipe_context::set_inlinable_constants`;
- `m9_context.c:m9_fs_variant` clones canonical NIR and caches the compiled
  result by those raw values.

Chrome9 should port that mechanism, not depend on application behavior or a
mapped BO remaining unchanged between callbacks.

### Integer representation is a mandatory part of the contract

Chrome9 advertises no native integer shader support.  Mesa therefore stores an
integer uniform as a float in the constant buffer.  The value five received by
`set_inlinable_constants` is `0x40a00000`, not integer bits `0x00000005`.

Substituting that word into an `int32` loop count without first applying
`nir_lower_int_to_float` would request more than one billion iterations.  The
working `m9x` sequence is the model:

1. set `.no_integers = true`, `.lower_uniforms_to_ubo = true`, and
   `.max_unroll_iterations = 32` in the fragment NIR options;
2. fix the integer element-index source of `load_ubo_vec4` after
   `nir_lower_int_to_float`, as `m9_specialise.c:ubo_offset_int` does;
3. identify the control dword after state-tracker uniform lowering;
4. substitute the raw `0x40a00000` value into the float-lowered clone;
5. fold and unroll.

If Chrome9 retains `load_uniform` temporarily instead of adopting UBO loads,
its custom address rule must be explicit: unpacked uniform base and offset are
vec4 units, and scalar component `c` addresses dword
`4 * (base + constant_offset) + c`.  The current generic leaf path accepts
only base zero and treats offset as a constant-register id; that is not a
general specialization ABI.  Converging on the already tested
`load_ubo_vec4` path is safer.

### Required screen and context contract

At link/finalize time:

1. enable `call_finalize_nir_in_linker` and install `finalize_nir`;
2. lower integers to the float-only form actually stored in CB0;
3. record only dwords that control loop termination or a wholly
   uniform-decided condition;
4. retain canonical, unspecialized NIR in the shader state.

At constant update time:

1. implement `set_inlinable_constants` for the fragment stage;
2. copy at most `MAX_INLINABLE_UNIFORMS` raw values into context state;
3. do not compile there: the shader may not be bound yet, and this callback is
   a value delivery mechanism, not a variant selection point.

Immediately before a draw:

1. build a key from shader identity and exactly
   `nir->info.num_inlinable_uniforms` raw values;
2. find or allocate a bounded variant (the proven sibling limit is 64);
3. clone canonical NIR with `nir_shader_clone(NULL, shader->nir)`;
4. substitute only the recorded dwords;
5. optimize to a fixed point with the following sequence;
6. require a single straight-line entrypoint and compile it normally;
7. cache success or failure for that exact key.

```c
do {
    progress = false;
    NIR_PASS(progress, nir, nir_opt_constant_folding);
    NIR_PASS(progress, nir, nir_opt_loop);
    NIR_PASS(progress, nir, nir_opt_loop_unroll);
    NIR_PASS(progress, nir, nir_opt_dead_cf);
    NIR_PASS(progress, nir, nir_opt_remove_phis);
    NIR_PASS(progress, nir, nir_opt_peephole_select,
             &(nir_opt_peephole_select_options){ .limit = 8 });
    NIR_PASS(progress, nir, nir_opt_copy_prop);
    NIR_PASS(progress, nir, nir_opt_algebraic);
    NIR_PASS(progress, nir, nir_opt_cse);
    NIR_PASS(progress, nir, nir_opt_dce);
} while (progress);
```

Do not clamp a trip count.  Counts at or below zero naturally produce zero
iterations.  Counts one through 32 may specialize.  A count above 32, a
non-constant address, an unbound value, an out-of-range dword, or a remaining
loop must produce a failed native variant.  A failed variant emits no partial
program.

Do not evict a variant while queued GPU work could still reference its program
storage.  Current Chrome9 draws complete synchronously, but a fixed cap with
failure after exhaustion remains correct if submission becomes asynchronous.

## CMP is selection, not predication

The exact recovered native CMP is:

```text
result = condition >= 0.0 ? true_value : false_value
native A = condition, C = true_value, B = false_value, subop = 9
```

It writes ordinary float lanes.  It does not create a boolean or predicate
register.  A generic NIR selector may fuse a comparison used only by `bcsel`:

| NIR condition | difference `d` | native selection |
|---|---|---|
| `fge(x,y)` | `x-y` | `CMP(d, true, false)` |
| `flt(x,y)` | `x-y` | `CMP(d, false, true)` |
| `fle(x,y)` | `y-x` | `CMP(d, true, false)` |
| `fgt(x,y)` | `y-x` | `CMP(d, false, true)` |

For finite operands, equality can be formed without a predicate register:

```text
d      = x - y
inner  = CMP(-d, true, false)
result = CMP( d, inner, false)
```

This is legal only after SUB, NEG/source modifier, CMP equality, signed zero,
and unordered/NaN behavior pass live silicon tests.  The captured glmark
`conditionals` comparison carries `preserve:inf`; the compiler must not simply
discard NIR exactness metadata.  Either prove the native behavior satisfies
the required domain or retain the rejection.

If-conversion is legal only when evaluating both arms eagerly preserves
observable behavior.  It covers the arithmetic assignments in conditionals,
cel, shadow, terrain, and the already-selected `unitvec` trees.  It does not
cover discard, memory, atomics, image writes, or a texture operation whose
conditional execution affects derivatives or faults.

## Resource closure exposed by the production compiler

Exact production objects in
[`chrome9-glmark-control-flow-corpus.tsv`](chrome9-glmark-control-flow-corpus.tsv)
close two important sizing questions:

- five `d = fract(3*d)` steps compile to 14 ALU records and an active resource
  tuple `(0,4,0,0)`;
- five conditional steps compile to 34 ALU records and tuple `(0,6,0,0)`.

The default five-condition shader therefore exceeds the driver's currently
advertised 32 ALU instructions even before accounting for Mesa's framebuffer-Y
transform.  Kernel and compiler stores allow 96 ALU records.  Raising the
Gallium limit is appropriate only when the generic scheduler can actually
allocate and submit that span.

The production five-step object reuses physical temporaries.  Chrome9's
current recursive NIR builder monotonically assigns a new virtual temporary,
while `chrome9_compile_ps_ir` requires a dense physical set and accepts a
resource bucket only up to six temporaries.  Uniform unrolling alone is
therefore insufficient.  The backend needs a liveness allocator that maps
SSA values onto freed r0..r11 slots before final resource-bucket derivation.
Scene-specific overwrite patterns are not acceptable.

The fixed three-light ideas shader may also exceed 96 ALU records after full
unrolling.  That is a separate program-size/scheduling problem: control-flow
elimination can be correct while the resulting native variant still refuses
for size.  Do not reintroduce unproven PS branch words to make it fit.

## Required corpus gates

Before enabling the path by default, an offline test must cover:

1. literal loop counts 0, 1, 5, 32, and 33;
2. uniform count five delivered as float bits `0x40a00000`;
3. uniform counts zero and negative producing a zero-iteration body;
4. out-of-range and changing values producing distinct cached variants;
5. a non-control uniform remaining dynamic and absent from the variant key;
6. five conditionals becoming straight-line SUB/FRC/CMP code;
7. `terminate_if` remaining rejected by the ordinary CMP path;
8. no loop/if/phi/call reaching native emission;
9. ALU count, temporary pressure, and command span checked after lowering;
10. cache exhaustion refusing without reusing live storage.

A live test must then compare zero, positive, negative, `+0`, `-0`, infinity,
and quiet-NaN CMP conditions against software reference output before generic
comparison selection is advertised.

## Evidence provenance

- production compiler:
  `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`;
- probe source: `tools/re/chrome9_pps_probe.c`, SHA-256
  `4299ac3cb182c9e26688dd40e63e9aed4fb86ce47c829717b2663b734f040c13`;
- exact compiler mode:
  `CHROME9_POSTPROCESS=2 CHROME9_RESOURCE_COUNTS=1` under `qemu-i386`;
- native-flow negative control:
  `windows-ps-gl2-next-corpus.tsv`, row `IF_C0_MOV_OC0_C1`;
- instruction and dispatcher analysis: `via-5.75-shader-re.md`, control-flow
  and instruction-flag sections;
- CMP/FRC/vPos objects: `chrome9-vpos-cmp-frc-contract.md` and
  `chrome9-vpos-cmp-frc-corpus.tsv`.

