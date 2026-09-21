# Chrome9 two-texture compiler callback provenance

Status: exact correction to the generic Linux pixel-compiler corpus.  The
long-used Xorg/fixed two-texture object remains valid hardware evidence and
has live silicon coverage; it is not the output of the production callback
order in the offline Linux wrapper.

## Exact discriminator

The PS 2.0 body is:

```text
texld r0,t0,s0
texld r1,t1,s1
mul   oC0,r0,r1
```

The production chip-5 selector table is:

```text
backend+137c = 001b6e10  token parser
backend+1380 = 001b5870  validator
backend+1384 = 001b8b80  processor/emitter
```

With those selectors, production raw output is:

```text
TAU 201001b0 211111b1
ALU 0140b6d0 30001053 00f00005 00000820
```

Configuration-2 produces:

```text
TAU       301001b0 311111b1
ALU       0140b6d0 30201053 00f20005 00000820
A90-A93   90018000 91000003 92020001 93000002
resources 2,2,2,2
```

The output retains ordinary physical texture temporaries r0/r1.  Their final
uses occupy native A and C, so configuration-2 sets A d2.bit17 and C
d1.bit21.  This agrees with the independently recovered whole-register
last-use finalizer.

The historical probe manually put the processor at +1380 and validator at
+1384.  Re-enabling exactly that non-production swap reproduces:

```text
raw TAU   201001b0 211ff1b1
raw ALU   0140b6d0 3003f000 00f00005 00000820

final TAU a01001b0 311001b1
final ALU 0140b6d0 3003f000 00f20005 00000820
A90-A93   90000000 91000001 92020001 93000002
```

Those final bytes are the familiar fixed/Xorg two-texture object.  Their
hardware validity is independently stronger than this compiler audit: the
fixed program has been integrated and exercised on Chrome9 HD.  The
correction is provenance and compiler architecture, not a request to replace
the live fixed program.

## Why the distinction matters

The two objects are indivisible schedules.  They differ in all of these
coupled properties:

- ordinary physical r0/r1 sources versus the fixed special source form;
- both TAU high/control fields;
- A90 and A91 phase/lifetime controls;
- source-C final-use location.

A real Mesa NIR backend must reproduce the production object when it uses the
production allocator.  It must not copy `3003f000` or `a01001b0` from the
fixed packet into an otherwise ordinary r0/r1 schedule.  Conversely, the
current fixed two-texture ABI must continue emitting its complete live-proven
tuple rather than substituting compiler fields piecemeal.

The first generic two-sampler compiler promotion therefore needs a guarded
silicon comparison:

1. keep the existing fixed/Xorg draw as the known-good bracket;
2. submit the complete production tuple above with identical sampler state,
   coords, textures, render/depth state, and vertices;
3. use asymmetric RGBA textures so every sampled lane and multiplication can
   be distinguished;
4. require exact pixel equality, guard integrity, ordered retirement, clean
   engine status, and a succeeding fixed draw;
5. promote only the whole production tuple after it passes.

Until then, production bytes are compiler evidence and the fixed tuple is the
runtime object.  They must have separate semantic program ids/corpus labels.

The correction also applies to the generic dependent-coordinate compiler
claim.  Production `tex0 r0; tex1 r1,r0; mul oC0,r0,r1` emits:

```text
TAU       301001b0 311111b1
ALU       01c00530 301c0053 00f00005 000d0460
          0140b6d0 30201053 00f20005 00000820
A90-A95   90018000 91000003 92020002 93000003 94000000 95000001
```

The old special-source/high-TAU alternating tuple is likewise a swapped-
callback/fixed-object result, not production generic allocator output.

## Unused and reused texture results

One-sample differentials close another unsafe shortcut.  A consumed single
TEXLD finalizes `201001b0 -> 301001b0`.  If its destination is never read,
the same raw TAU instead becomes `b01001b0`, while the unrelated color MOV is
unchanged.  Thus TAU bit31 participates in target lifetime/schedule handling;
it is not a fixed "first of two samples" bit.

When one sampled r0 is read twice, the first ALU source is unmarked and the
terminal read receives A d2.bit17.  The unused intermediate r1 destination
gets d3.bit29.  TAU remains `301001b0`.  This independently joins texture
results to the same physical-temp final-use model without reducing TAU high
bits to the ALU rule.

TAU finalization remains schedule-specific.  Do not implement any constant
OR formula for bits31/28; use only closed complete tuples until the phase
finalizer itself is reproduced.

## Evidence

All four raw/finalized rows and controls are retained in
[chrome9-two-texture-callback-provenance-corpus.tsv](chrome9-two-texture-callback-provenance-corpus.tsv).
They use the checked-in Linux probe and `CHROME9_POSTPROCESS=2`.  The swapped
row is reproducible only with `CHROME9_NONPRODUCTION_CALLBACK_ORDER=1`.
No hardware was touched in this audit.
