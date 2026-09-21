# Chrome9 FLR expansion and FRC boundary

FLR is not a one-record native successor to FRC.  Independent Windows and
Linux disassembly shows that the internal pixel FLR emitter allocates one
temporary, appends a subop-e FRC record, appends a second subop-9 record, and
then releases the temporary.  Exact control-flow anchors are retained in
[`chrome9-flr-expansion-static.tsv`](chrome9-flr-expansion-static.tsv).

This is a static cross-driver result.  The public D3D ps_2_0 tokenizer has no
FLR token, and the internal OpenGL grammar path has not yet been invoked with
a production-initialized allocator/finalizer.  There is consequently no
dynamic byte-exact FLR stream and no silicon semantics claim here.

## Windows and Linux emitter structure

The Windows internal emitter is `0x10216a10`.  It allocates a temporary at
`0x10216a37`, constructs the first record with subop e (`or eax,0x000e0020`
at `0x10216abf`), appends it at `0x10216b1c`, constructs the second record
from temporary/source/destination state with subop 9, appends it at
`0x10216bf9`, and releases the temporary through the allocator vtable at
`0x10216c0c`.

The symbolized Linux twin is `stmPPS_FLR_ICD_PS20_invi` at `0x00164290`.
It allocates at `0x001642f9`, constructs subop e with
`or eax,0x000e0020` at `0x001643b0`, appends the first record at
`0x001643dd`, appends the subop-9 second record at `0x001644bd`, and releases
the temporary at `0x001644cc`.  Both emitters therefore have the same
allocate/two-appends/release shape and principal subops e then 9.

The second record is essential compiler output.  A shared subop number does
not by itself define the precise arithmetic, compare sense, exceptional-value
handling, or dependency bits.  Do not replace the two-record FLR lowering
with FRC alone or infer `x-fract(x)` packing from the high-level identity.

## Mesa semantic boundary

NIR `ffloor(x)` is floor toward negative infinity.  NIR `ffract(x)` is
`x-floor(x)`, so the algebraic identity `floor(x)=x-fract(x)` is valid over
ordinary finite reals.  It does not make FLR a one-record operation: the
vendor compiler explicitly preserves the second stage, and its rounding for
large values, signed zero, denormals, NaN, and infinity is unmeasured.

For terminal `ffloor(c0)` only, Mesa can constant-fold finite job-local c0 on
the CPU and submit the already-supported MOV(c0) shape with rewritten values.
That is not native FLR support and must obey the MOV constant/FP contract.
For v0, normalized interpolation does not make floor uniformly constant:
values below one map to zero while an exact endpoint one maps to one.  Do not
fold `ffloor(v0)` globally.

## Fail-closed policy and future oracle

Reject native FLR in the current one-record UAPI.  Also reject recognizing a
FRC-plus-subtract NIR graph as FLR, because multi-record allocation and the
subop-9 stage are not admitted.  Source modifiers/swizzles, partial masks,
SAT, scale, temporary consumers, and explicit FP controls remain outside the
static evidence.

A future production wrapper must obtain the exact two records from each
driver for c0 and v0 identity plus negative and boundary values, then compare
Linux configuration-2 dependency bits.  The first live vectors should be
`(+0.25,+1.0,-0.25,-1.0) -> (0,1,-1,-1)` and the mantissa-edge cases already
used by the FRC audit.  Negative outputs require A33 or a signed/float target;
UNORM readback alone clamps away the evidence.

Every live experiment uses command 19's mandatory fresh D16 target, depth
clear 1, LESS/write at z=0.5, COPY, exact command-word checks, target/depth
guards, fences, reset/error counters, and known-good draws before and after.
No FLR key should be advertised until the exact production stream, dependency
finalization, negative-floor rule, lane behavior, and exceptional policy all
pass.

