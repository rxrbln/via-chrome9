# Chrome9 generic floor lowering without special scratch

Status: Linux 5.75 production compiler and configuration-two exact; negative
FRC, full generic floor graph, and Mesa GLSL lowering live-qualified on VX900.
This is the implemented straight-line lowering for NIR `ffloor`, not
authorization to expose native internal FLR opcode 103.

## Why the native FLR expansion is unsuitable

The proprietary internal FLR emitter uses compiler-created special scratch
r12.  Its production object reports active tuple `(0,13,0,0)`, selects
A90=`90040000`, and explicit-TEMP compositions time out in the recovered
Windows compiler.  Those constraints do not fit the structured UAPI's
ordinary r0--r11 SSA allocator.  They are documented in
[`chrome9-flr-production-audit.md`](chrome9-flr-production-audit.md).

The mathematical identity

```text
floor(x) = x - fract(x)
```

uses only the already-encoded FRC and SUB operations and an ordinary logical
TEMP.  It preserves the NIR definition exactly if native FRC is floor-based
for negative finite inputs; that numerical prerequisite remains a silicon
gate.

## Exact two-record object

The PS 2.0 stream

```text
ffff0200
03000013 000f0000 30e40000                 # FRC r0,T0
04000003 000f0800 30e40000 00e40000        # SUB oC0,T0,r0
0000ffff
```

compiled through the Linux 5.75 production H5 frontend and configuration two
to:

```text
active-resource-counts: 1 1 1 0
01c00530 301c0053 00f0c005 000e0020
82c00530 30140924 00f20005 00000820
A90=90058001 A91=91000000 A92=92000002 A93=93000000
```

The final SUB reads original T0 through A and releases the ordinary TEMP
through B (`d0.bit25`).  There is no special id12, no TAU record, and no
hidden phase transition.  The active tuple and A90 bucket are the same
ordinary direct-T0 resource class already used by the generic program path.

## Guarded numeric discriminator

A terminal floor result is negative for half of the useful boundary vector,
so final UNORM conversion would hide a truncation error.  The following
three-record observation keeps every expected lane nonnegative:

```text
FRC r0,T0
SUB r1,T0,r0
MAD oC0,r1,c0.xxxx,c0.yyyy
```

Linux production configuration two gives:

```text
01c00530 301c0053 00f0c005 000e0020
82c00530 30140924 00f20005 00000060
00802490 30080000 00f20105 00000820
active=(1,2,1,0)
A90=90058001 A91=91000000 A92=92000003 A93=93000000
```

Set `c0=(.25,.5,0,0)` and flat
T0=`(-1.75,-.25,.25,1.75)`.  Correct floor values `(-2,-1,0,1)` become
`(0,.25,.5,.75)`, numeric A8R8G8B8 host word `bf004080`.  A truncate-based
fraction would instead produce integer vector `(-1,0,0,1)`, transformed to
`(.25,.5,.5,.75)` and distinct host word `bf408080`.  Check guarded BGRA8888 and D16 allocations,
triangle exterior, exact submitted words, reset/error counters, and a
succeeding known-good draw.

Before running the three-record floor graph, directly gate FRC with
T0=`(-1.75,-.25,.25,1.75)`: GLSL/NIR fract expects
`(.25,.75,.25,.75)`, host word `bf40bf40`.  This isolates FRC's negative
floor rule from the later SUB/MAD records.

That direct `--frc-negative` gate has now passed on the 192.168.2.60 VX900:
exact `bf40bf40`, correct D16, intact color/depth guards and triangle
exterior.  The following known-good EGL draw passed and dmesg remained clean.
Native FRC therefore uses the GLSL/NIR floor-based negative rule for these
finite values.  The next promotion gate was the complete explicit FRC -> SUB
floor graph with the observation MAD above, not internal opcode 103.

The complete `--floor-negative` observation then passed exact `bf004080` on
the same target, with correct D16, intact color/depth guards and exterior, and
a passing known-good EGL draw afterward.  This excludes the truncation result
`bf408080` and qualifies the tested FRC -> SUB floor chain plus observation
MAD on finite negative and positive lanes.

## Compiler policy

Lower `ffloor` only after negative FRC passes.  Emit explicit FRC plus SUB
through the normal SSA allocator; never select internal FLR opcode 103 or
reserve special r12.  Keep non-finite input rejected under the current
finite-only contract.  The observation MAD uses one constant id in both
constant source slots and therefore stays inside the live-proven two-distinct
constant-per-record limit.

## Mesa integration

Before the live closure, Chrome9's NIR options enabled `lower_fdiv`,
`lower_flrp32`, `lower_fmod`, `lower_fsat`, and `lower_sincos`, but left
`lower_ffloor` false.  The generic fragment builder accepts `nir_op_ffract`
as native FRC and intentionally has no direct `nir_op_ffloor` case.  A
surviving floor was therefore rejected, and `lower_fmod` could create an
unsupported floor dependency.

Mesa's standard NIR algebraic rule already defines the desired 32-bit
lowering when `options->lower_ffloor` is true:

```text
ffloor(a) -> fsub(a, ffract(a))
```

Mesa commit `7a56ec0` now sets `.lower_ffloor = true` in the Chrome9 NIR
options; parent integration commit is `4927257`.  A live GLSL shader with a
uniform negative floor input changed from unsupported to a five-ALU native
program and passed its negative-pixel check.  The implementation correctly
uses standard NIR FRC+SUB lowering and did not add native-FLR opcode 103.
Focused compile coverage should continue to require one FRC plus one SUB for
the floor subgraph and verify that lowered `fmod` contains no surviving
`ffloor` before semantic translation.
