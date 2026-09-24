# Chrome9 generic floating-point comparison boundary

## Result

The proprietary compilers do not justify lowering every NIR comparison to a
subtraction followed by native CMP.  They provide an exact, two-record SGE
expansion and a separate two-record internal SLT emitter.  Only SGE currently
has an executable frontend object.  Native CMP is live-qualified for ordinary
finite negative, positive, `+0`, and `-0` conditions, but subtraction
underflow, denormal handling, infinities, and NaNs remain unqualified.

Consequently the generic backend must continue to reject arbitrary
`flt`/`fge`/`feq`/`fneu` and their `bcsel` users.  A finite-domain shader may
use `SUB` plus CMP only after its subtraction result is proven to preserve the
ordered comparison.  Merely knowing that the two operands are finite is not
enough: a nonzero difference can underflow or be flushed to zero.

## Production SGE object

Linux 5.75 `stmPPS_SGE_PS20_invi` is at virtual address `0x0016ad60`.
It allocates a temporary through the callback at object offset `+0x138c`,
packs the first source-dependent record, and appends it at
`0x0016afe2..0x0016afed`.  The second record starts from hard-coded bases
`d0=0140b6d0` at `0x0016aff4` and `d1=30140924` at `0x0016affb`; it is appended
at `0x0016b075..0x0016b084`.  The temporary is released through callback
`+0x1394` at `0x0016b084..0x0016b091`.  The Windows counterpart is
`VTGUModeDX32A.dll:0x10219700` and has the same allocate/two-append/finalize
shape.

Linux configuration-two finalization gives these complete records:

```text
SGE oC0,c0,c1
  00810538 30080924 00f08005 00000020
  0140b6d0 30140924 00f20005 00090820
  A90=90058000 A91=91000000 A92=92000002 A93=93000000

SGE oC0,T0,c0
  00800538 302c0924 00f0c005 00000020
  0140b6d0 30140924 00f20005 00090820
  A90=90058001 A91=91000000 A92=92000002 A93=93000000

SGE oC0,c0,T0
  02c00538 30080924 00f08005 00000020
  0140b6d0 30140924 00f20005 00090820
  A90=90058001 A91=91000000 A92=92000002 A93=93000000

SGE oC0,T0,T1
  02c10538 302c0924 00f0c005 00000020
  0140b6d0 30140924 00f20005 00090820
  A90=90058003 A91=91000000 A92=92000002 A93=93000000
```

The first row is `src0 + -src1` into the allocated temporary.  Decoding the
second row fixes the apparently opaque constants:

```text
A: file 0, id 0, XYZW, release       temporary result
C: file 5, id 0, swizzle 4444       native ONE
B: file 5, id 0, swizzle 5555       native ZERO
subop 9, full oC0 write
```

Thus production SGE is exactly a difference followed by the native
`difference >= 0 ? 1 : 0` family operation.  The compiler object proves the
record shape, operand order, resource tuple, and terminal dependency.  It
does not prove the hardware's NaN, denormal, infinity, or signed-zero result.

The current semantic UAPI cannot reproduce that second record byte-for-byte:
its source files encode TEMP as native file 0, CONSTANT as file 2, and
VARYING/MISC as file 3.  Literal ONE/ZERO swizzles do not select native file
5.  A guarded exact SGE oracle therefore needs either a semantic SGE opcode
whose kernel template owns both records, or an internal kernel selftest.  An
`ADD; CMP` program with uploaded constants is useful arithmetic evidence but
is not the proprietary SGE oracle.

## Internal SLT is now byte-exact, but not numerically qualified

Linux `stmPPS_SLT_ICD_PS20_invi` at `0x001644e0` independently allocates a
temporary and appends two records.  Its second record starts from
`d0=01409240` at `0x00164699` and `d1=30140b6d` at `0x001646a0`, so it is not
an alias for SGE with exchanged arguments.  The Windows internal counterpart
is `VTGUModeDX32A.dll:0x10216550`, reached as `STM_SLT_ICD` internal opcode
107.

The tested Windows PS 2.0 tokenizer crashes before dispatching direct SLT, and
the normal Linux public frontend rejects it.  The checked-in Linux probe now
has a research-only `CHROME9_INTERNAL_SLT` path which constructs exactly one
normalized opcode-107 record and calls the named internal emitter.  The
public optimizer cannot be applied to that isolated record because it
dereferences parser-owned neighboring IR; configuration-two dependency
finalization itself succeeds.  The resulting isolated terminal records are
listed below; the two-varying row is malformed and is not an executable
oracle.

```text
SLT oC0,c0,c1
  00810538 30080924 00f08005 00000320
  01409240 30140b6d 00f20c05 00090820
  active=(0,13,0,0), A90=90040000, A92=92000002

SLT oC0,T0,c0
  00800538 302c0924 00f0c005 00000320
  01409240 30140b6d 00f20c05 00090820
  active=(1,13,1,0), A90=90040001, A92=92000002

SLT oC0,c0,T0
  02c00538 30080924 00f08005 00000320
  01409240 30140b6d 00f20c05 00090820
  active=(1,13,1,0), A90=90040001, A92=92000002

SLT oC0,T0,T1
  00ff0538 302c0924 00f0c005 00000320
  01409240 30140b6d 00f20c05 00090820
  active=(1,13,1,0), A90=90040001, A91=91000001, A92=92000002
```

### Two-varying probe limitation

In the isolated `T0,T1` row, the first record's native B source decodes as
file 3, id **63**, with no release marker.  The corresponding production SGE
`T0,T1` row encodes B as file 3, id **1**, with a release marker
(`02c10538` rather than `00ff0538`).  The SLT row's active tuple also
accounts for only one direct coordinate.  Id 63 is outside the eight-varying
semantic ABI and cannot stand for `T1`; do not submit or byte-match this row
as a valid two-varying program.  A repeat of the checked-in probe with
`CHROME9_TRACE_REGS=1` showed `tex-manager: remap logical=0 -> 0` followed by
`tex-manager: remap logical=1 -> -1`; the latter is truncated to the six-bit
native id 63.  The isolated emitter call never registered T1 with the texture
slot manager, so this is a probe-setup failure, not evidence of a silicon
two-varying limit.  The opt-in `CHROME9_INTERNAL_SLT_REGISTER_T1=1` probe
variant registers both inputs before `stmSetRegMgr_invi`, asserts `T1 -> 1`,
then runs configuration-two finalization and emits:

```text
SLT oC0,T0,T1 (both inputs registered, configuration 2)
  02c10538 302c0924 00f0c005 00000320
  01409240 30140b6d 00f20c05 00090820
  active=(1,13,1,0), A90=90040003 A91=91000000 A92=92000002
```

The corrected B source is file 3, id 1, with its release marker, matching
production SGE's two-varying linkage.  This is an offline compiler oracle,
not a silicon test; native SLT numeric semantics remain unqualified.

The first record computes `src0 + -src1` into special scratch r12.  Decoding
the second gives A=r12/release, C=native ZERO, B=native ONE, subop 9, so the
symbolic operation is exactly `difference >= 0 ? 0 : 1`.  This independently
proves the strict arm order; it is not SGE with exchanged source tokens.
Special-r12 allocation explains the active temporary high-water 13 and the
`90040000` resource bucket, matching the established FLR/POW/NRM special
scratch family.

The constant, one-varying, and corrected two-varying rows provide compiler-
shape oracles, but they remain silicon-unqualified and use r12, which the
current generic allocator does not expose.  The malformed historical row
provides no execution oracle.
Do not invent a one-record SLT or silently substitute SUB+CMP.  A future
semantic SLT opcode must own this two-record expansion, special scratch
reservation, and dependency finalization in the kernel.

## NIR semantics that must be preserved

Mesa 26.2.2 defines the relevant NIR operations in
`src/compiler/nir/nir_opcodes.py:927..934`:

```text
flt   src0 <  src1
fge   src0 >= src1
feq   src0 == src1
fneu  src0 != src1
```

The ordinary forms are ordered for `<`, `>=`, and `==`; `fneu` is true for an
unordered NaN input because C/Python `!=` is true there.  NIR separately has
the explicit unordered `fltu`, `fgeu`, `fequ` and ordered-not-equal `fneo`
forms.  Collapsing those distinctions in a Chrome9 lowering is incorrect.

Signed zeros compare equal.  The live CMP gates show that both `+0` and `-0`
conditions select the nonnegative arm, which is compatible with a difference
that is already known to be zero.  They do not establish which zero sign a
Chrome9 SUB produces, nor whether a subnormal difference survives.  NaN is
also unresolved: `+Inf - +Inf` produces NaN even though both operands are
non-NaN, while SGE/CMP native unordered behavior is unknown.

The first conservative fused-select mappings are therefore only algebraic
candidates:

```text
bcsel(fge(x,y), t, f): d = x - y; CMP(d, t, f)
bcsel(flt(x,y), t, f): d = x - y; CMP(d, f, t)
```

They are exact only when the compiler proves that `d` is non-NaN and that
Chrome9 subtraction returns zero exactly iff `x == y`, otherwise a nonzero
value with the mathematical sign of `x-y`.  No such proof exists for general
varyings or uniforms.  The current generic Mesa lowering correctly falls
through as unsupported for comparisons at
`chrome9_context.c:3086..3153`, and it rejects any explicit NIR float-control
execution mode at `chrome9_context.c:3273..3275`.

`feq` and `fneu` need more than one sign test.  `ABS(x-y)` followed by a
negative/equality trick still inherits subtraction underflow and unknown NaN
behavior.  Two directional comparisons can characterize equality only after
SGE/SLT unordered behavior and boolean combination are proven.  Both
operations remain rejected; there is no source-backed one-CMP shortcut.

## Per-instruction float controls are part of the acceptance boundary

Rejecting only `nir->info.float_controls_execution_mode` is insufficient for
new comparison lowering.  `nir_alu_instr::fp_math_ctrl` carries the local
contract.  Mesa 26.2.2 defines `nir_fp_preserve_inf` and
`nir_fp_preserve_nan` to make an ALU result defined when an input or result is
an infinity or NaN (`src/compiler/nir/nir.h:1553..1582`).  Floating-point
comparison opcodes admit exactly those two local flags through
`binop_compare_all_sizes` (`nir_opcodes.py:665..670`).  The Chrome9 generic
ALU switch currently neither supports comparisons nor checks the local flags
(`chrome9_context.c:3088..3155`); its shader-wide check at
`chrome9_context.c:3274..3277` does not discharge that obligation.

The captured glmark2 programs show why this distinction matters:

* `conditionals` has `fge(ffract(x), 0.5) // preserve:inf`.  For every finite
  input, `ffract(x)` is in `[0,1)`.  The candidate subtraction is therefore
  in `[-0.5,0.5)`.  It cannot overflow.  It also cannot turn a representable
  nonzero distance from `0.5` into a subnormal: around `0.5` the adjacent
  binary32 spacing is at least `2^-25`, far above the smallest normal.  For
  operands in `[0.25,1)`, Sterbenz exact subtraction applies; below `0.25`
  the result magnitude is at least `0.25`.  An infinite comparison operand
  is impossible after a valid finite `ffract`; a NaN path is explicitly
  undefined because this instruction does not carry `preserve:nan`.
  Consequently this one captured predicate has a complete finite-domain
  proof for production SGE/SUB+CMP.  The proof belongs to this expression,
  not to generic `fge`.
* `ideas` compares a varying Z and a uniform W against zero with
  `feq ... // preserve:inf,nan`, then combines the predicates with `iand` and
  three `bcsel` operations.  Those sources have no bounded finite range in
  the captured NIR, and ordered equality must return false for NaN.  Neither
  the current CMP silicon corpus nor the proprietary SGE/SLT object bytes
  establish that behavior.  This shader must remain fail-closed.

An implementation-ready first slice is therefore deliberately narrow:
recognize only `bcsel(fge(ffract(v), 0.5), t, f)` with the exact local-control
contract above, lower it through the production comparison expansion, and
leave all other comparisons unsupported.  A more general range analysis may
later accept expressions whose difference is independently proven normal,
non-NaN, and sign preserving.  It must inspect `alu->fp_math_ctrl` for every
comparison it transforms.

### Five-step guarded conditionals gate

`tools/re/chrome9_conditionals_guard_smoke.c` is the bounded successor to the
single-CMP tests.  It uses only already-defined semantic UAPI operations and
does not require SGE exposure.  Each of five dependent steps emits:

```text
condition = SUB(current, 0.5)
true      = FRC(MUL(current, 2.0))
false     = FRC(MUL(current, 3.0))
current   = CMP(condition, true, false)
```

This is the same six-record step recovered in
`chrome9-glmark-control-flow-corpus.tsv` for `conditionals_body_5`.  The raw
production program has 34 records including its three-record position prefix
and terminal move, with active tuple `(0,6,0,0)`.  The guarded test replaces
that already-qualified prefix with a direct T0 seed, so its candidate is 31
ALUs and isolates the five SUB/MUL/FRC/CMP steps plus allocator lifetime
reuse.  It deliberately allocates 30 monotonically numbered virtual
temporaries; the kernel must recycle physical r0--r11 by last use rather than
accepting it only because the test names a small fixed register set.

The exact T0 seed is RGBA `(.125,.375,.25,.75)`.  A MOV control must store
host BGRA `bf206040`.  Five steps produce exact binary fractions
`(.375,.125,0,0)`, host BGRA `00602000`; across those lanes both CMP arms are
taken.  Both draws require D16 near `8000`, untouched exterior pixels, and
intact leading/trailing BO guard pages.  Both draws passed on VX900: the
31-ALU candidate produced the expected pixels, D16, exterior, and guard
values.  The MOV control first needed its UAPI metadata corrected to use
zero constant count and a null constant pointer; its earlier `EINVAL` was a
pre-submission validation failure, not a GPU fault.

The constant-only CMP oracle with three distinct constant registers returned
`EINVAL` at the kernel's known constant-port guard.  A TEMP-backed CMP
oracle passed negative, positive, +0, and -0 condition lanes; a dynamic vPos
threshold oracle passed as well.  These finite-domain checks do not qualify
NaN or arbitrary comparison lowering.

## Mesa finite-range lowering and glmark validation

Mesa now proves the compared value lies in `[0,1]` by a bounded walk over
defined `ffract` results and selections whose two arms have that same proof.
It accepts scalar `bcsel(fge(value, threshold), true, false)` only when the
threshold is a finite FP32 constant in `[0.25,0.75]`, the comparison has no
local control except optional `preserve:inf`, and the FRC leaves have no local
FP preservation controls.  On the defined finite domain, subtracting such a
threshold preserves the comparison sign and cannot overflow or flush a
nonzero result to zero.  Mesa emits `SUB(value,threshold)` then the native
three-source CMP.  Other comparison shapes still fail compilation.

The installed `conditionals:fragment-steps=5` scene compiled to 40 semantic
fragment instructions (the five six-record condition/select steps plus the
OpenGL position prefix and output assembly).  One bounded 320x240 frame
submitted without an ioctl error.  At glmark2's 800x600 validation size,
`glmark2 --validate --off-screen --frame-end=readpixels -b
conditionals:fragment-steps=5` reported `Validation: Success`.  glmark2
reported `Validation: Unknown` for the unlisted one-step/default option
strings; those are not correctness evidence.  The five-step result and the
31-ALU guarded raw oracle together qualify this finite FRC/select graph,
not general GLSL comparisons or exceptional values.

## Guarded qualification sequence

An exact semantic SGE successor should expand one validated instruction to
the two production records above and must pass, in order:

1. a no-comparison MOV control with the same BGRA8888, D16, guarded BOs,
   geometry, constants/varyings, and exterior checks;
2. lanes covering `<`, `==`, `>`, `+0` versus `-0`, and `-0` versus `+0`;
3. smallest positive and negative subnormal differences and exact
   cancellation, while recording the hardware's flush behavior;
4. `+Inf`, `-Inf`, and equal-infinity pairs;
5. quiet NaN in each source position, checked against each NIR ordered and
   unordered operation rather than against D3D compiler naming.

The first two gates are enough to qualify ordinary finite SGE materialization
for bounded users.  They are not enough to expose generic NIR comparisons.
The exceptional gates determine whether a general lowering is possible or
whether Mesa must retain a fail-closed range/fast-math restriction.
