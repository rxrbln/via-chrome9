# Chrome9 `ideas` POW20 vectorization oracle

Status: source-exact NIR analysis and a fresh, hardware-free Linux 5.75
production compiler/configuration-two object.  No GPU was used.  This closes
the instruction-count and native-lane question; it does not close the full
lamp allocator or the independent FEQ/unit-vector gate.

## Result

The three `ideas-lamp-lit.frag` powers may be packed into XYZ and evaluated by
one componentwise five-MUL chain.  This is a generic SLP transformation, not
a scene or shader-hash special case:

```text
b    = vec3(base0, base1, base2)
b2   = b * b
b4   = b2 * b2
b5   = b4 * b
b10  = b5 * b5
b20  = b10 * b10
```

The exact conservative native accounting is:

| part | records |
|---|---:|
| pack three unrelated scalar definitions into X/Y/Z | 3 MOV |
| componentwise integer-power chain | 5 MUL |
| unpack | 0 |
| **total replacing the three scalar POW20 chains** | **8** |

Each original scalar POW20 chain is five MUL records, so the replacement
saves `15 - 8 = 7` ALU records.  Its results need no extraction MOV: the
three existing scalar consumers can read the final temporary as `xxxx`,
`yyyy`, and `zzzz` respectively.  A terminal MOV appears only in the isolated
compiler oracle below so the program has an observable output.

Applied to the count in
[`chrome9-ideas-lighting-capacity-boundary.md`](chrome9-ideas-lighting-capacity-boundary.md),
this changes the pre-FEQ lamp bound from 103 to 96 records.  Applying the four
independent ADD/FNEG-to-SUB fusions as well changes 99 to 92.  Neither number
is a complete executable-lamp claim: FEQ/unit-vector lowering remains guarded,
and the full combined graph must still pass the twelve-physical-temporary
allocator.

## NIR and GLSL legality

The captured post-option NIR has the three scalar operations `%46`, `%69`,
and `%96`.  Their bases are `%45`, `%68`, and `%95`, each produced by
`fmax(0.0, dot(...))`; their common exponent is the exact float32 constant
`0x41a00000` (`20.0`).  There is no data edge among the three base/power
subgraphs.  The corresponding source is the unrolled three-light loop, not
three calls created by a compiler-side pattern key.

Two transformations must be distinguished:

1. Integer strength reduction replaces each `fpow(base, 20.0)` with the
   five operations above.  The captured `fpow` instructions carry fast-math
   control zero: they are not marked `no_transform`, `no_reassoc`, or exact,
   and do not request preservation of signed zero, infinity, or NaN.  Their
   bases are also explicitly non-negative in GLSL.  The current Chrome9
   backend already performs this integer-power lowering.
2. SLP then executes the same five operations in three components.  NIR
   vector floating-point ALU is componentwise.  Per lane, both operand order
   and the sequence `2,4,5,10,20` remain unchanged.  SLP therefore adds no
   arithmetic reassociation and creates no cross-lane dependency.

A generic pass must still require all grouped chains to have the same bit
size, exact integer exponent, multiplication topology, and floating-point
control at every paired operation.  It must reject a `precise`/`no_transform`
power before integer strength reduction, and it must not combine chains whose
generated MUL controls differ.  This rule is sufficient for the captured
lamp but is not permission to vectorize arbitrary `fpow` operations.

## Exact Linux production object

The production compiler input uses direct `T0.x`, `T1.x`, and `T2.x` as
three generic unrelated bases.  It performs three disjoint partial writes to
one temporary, the five vector MULs, then a full output MOV.  The full output
intentionally exercises the compiler's established `xyz1` fill for the
undefined W lane; the POW result itself uses XYZ only.

```text
ps_2_0
mov r0.x, T0.x
mov r0.y, T1.x
mov r0.z, T2.x
mul r1.xyz, r0.xyz, r0.xyz       # b2
mul r2.xyz, r1.xyz, r1.xyz       # b4
mul r3.xyz, r2.xyz, r0.xyz       # b5
mul r4.xyz, r3.xyz, r3.xyz       # b10
mul r5.xyz, r4.xyz, r4.xyz       # b20
mov oC0, r5.xyz1
```

The exact PS 2.0 token stream is:

```text
ffff0200
03000001 00010000 30000000
03000001 00020000 30000001
03000001 00040000 30000002
04000005 00070001 00e40000 00e40000
04000005 00070002 00e40001 00e40001
04000005 00070003 00e40002 00e40000
04000005 00070004 00e40003 00e40003
04000005 00070005 00e40004 00e40004
03000001 000f0800 00e40005
0000ffff
```

Linux 5.75.32.87a `stmPPSCompile_invi`, followed by
`stmPPS_PostTranslationSetting_invi(object, 2)`, emits:

```text
01c00530 d01c0053 0012c016 000d0020
01c00530 d01c0053 0022c1a2 000d0020
01c00530 501c0053 0042c2b4 000d0020
0140b6d0 50000055 00700005 00000060
0140b6d0 50001055 00720105 000000a0
0140b6d0 50200055 00720205 000000e0
0140b6d0 50003055 00720305 00000120
0140b6d0 50004055 00720405 00000160
01c00530 401c0053 00f20505 000d0820
```

The finalized active-resource tuple is `(3,6,3,0)`.  There are nine ALU
records, no TAU records, and no float constant or immediate slot: exponent
20 is a compile-time match and disappears from the native object.  The exact
controls are:

```text
90048007 91000000 92000009 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

Packed little-endian SHA-256 values are:

| stream | SHA-256 |
|---|---|
| PS 2.0 tokens | `ebd9684464286ba9a5058c6ff1e6c592902bbaddf81db1980e9e57760006c52e` |
| nine native ALU records | `7dba216f97032032e9915b2ada071b36a41600632add46f109fe7c56f973b7cc` |
| A90--A9B controls | `46137161c79af334803c890ad15f366704a56acc2b26bf02b561d05a43605d60` |
| ALU then controls | `c6e6af79b51d974bff2ed724c4d264428d7389203ba5a0763d0cb298d0a2b7c3` |

The native decode is lane-local.  The three MOVs have masks X, Y, and Z and
source selectors `x000`, `0x00`, and `00x0`.  Every MUL has mask XYZ and both
live sources select `xyz0`.  There is no permutation or lane-reduction
instruction.  Configuration two marks the final uses in the expected order:
`r1` dies in the `b4` record; `r2` and packed base `r0` die in `b5`; `r3`
dies in `b10`; `r4` dies in `b20`; and `r5` dies in the output MOV.  The
vendor allocator retains physical temporaries 0 through 5, explaining the
temporary high-water of six.

This tuple is exact for the isolation program, not the full lamp.  Moving
the three bases together extends the first and second base lifetimes until
the third base is available, while their surrounding light accumulators are
also live.  Only compilation of the transformed whole NIR can establish its
physical high-water.  Instruction capacity alone must not be used as an
allocator proof.

## Windows evidence boundary

No fresh whole-object Windows result is claimed here.  Existing production-
initialized Windows differentials independently byte-match Linux for the
constituent partial TEMP MOV masks, TEMP swizzles, ordinary MOV/MUL record
templates, physical remapping, and configuration-two last-use rules.  Static
analysis also shows byte-normalized D3D/OpenGL copies of the MOV and MUL
emitters and finalizer.  Those facts make the Linux object unsurprising, but
they do not substitute for running this nine-record stream through the
Windows production wrapper.  Linux remains the authoritative executable-form
oracle for this note.

## Hazards and compiler boundary

The optimization must be a general same-operation SLP pack with these
fail-closed conditions:

- only independent scalar definitions may occupy different components;
- a component must never source another packed component;
- bit size, exponent value, multiplication order, destination controls, and
  FP controls must match;
- each packed lane must be read only from the corresponding lane; no DP,
  reduction, cross-product, or mixed swizzle is allowed in the chain;
- do not materialize NEG or ABS as a disputed native source modifier;
  the captured bases are unmodified MAX results;
- build complete native records for each partial destination mask rather than
  patching a full-mask template, because masked-off source selectors differ;
- use disjoint destination masks X/Y/Z.  The current kernel admits disjoint
  component definitions but correctly rejects overlapping WAW;
- retain the final packed value and use source swizzles for scalar consumers;
  inserting three extraction MOVs would erase most of the capacity win;
- recompute liveness and physical allocation after scheduling; do not assign
  the isolation object's r0--r5 numbers in the NIR lowerer.

The pack itself has a real silicon consideration distinct from vector MUL:
it relies on three sequential disjoint writes preserving previously written
components of one temporary.  Vendor compilation proves the intended object,
and the current semantic validator models disjoint component definitions,
but the exact three-write merge still deserves its own guarded pixel before
the transform is used as a capacity escape hatch.

## Smallest tests

The smallest offline compiler test should construct three unrelated scalar
32-bit bases, build one vec3, execute the exact five-MUL chain, and write
`xyz1`.  It should assert:

1. semantic IR masks `1,2,4,7,7,7,7,7,15`;
2. exactly three MOV packs, five MULs, zero unpack MOVs, and one terminal MOV;
3. no immediate/constant allocation for exponent 20;
4. successful kernel-ABI validation of the disjoint partial definitions;
5. the exact nine Linux records, tuple, controls, and hashes above;
6. refusal to group a different exponent, FP-control mismatch, dependency
   between bases, or a lane-crossing consumer.

A later guarded silicon discriminator can use constant scalar bases
`(0.875, 0.9375, 0.96875)`.  Float32 evaluation in the exact chain gives
approximately `(0.06920876, 0.27505881, 0.52994937)`, producing distinct
UNORM8 channel bytes near `(18,70,135)`, with alpha one from `xyz1`.  Require
an interior rectangle, untouched exterior, destination/depth guards, engine
error and reset counters, and a succeeding known-good draw.  This one image
simultaneously distinguishes lost earlier pack writes, lane swaps, accidental
scalar broadcast, wrong chain order, and failure of the terminal lane
swizzles.
