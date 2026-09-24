# ioquake3 GL2 pshadow compiler boundary

Status: exact default-source link and offline Chrome9 rejection audit,
2026-09-24.  No program described here has been submitted to the GPU.

## Exact default object

The audited ioquake3 sources are:

```text
pshadow_vp.glsl  b4853fb40cfd52c9f343f0691516f6a2e11050e865a8fa1443321c8e207e11a9
pshadow_fp.glsl  4e08509c4b48ae39c50dfe9f845ba37f168a945400595ca1d11abff247de021d
```

This is the default form without `USE_DISCARD`, `USE_PCF`, or
`USE_SOLID_PSHADOWS`.  It uses one ordinary 2D sampler, two smooth vec3
varyings packed across two input slots, and five vec4 constant registers.  It
does not need a shadow-comparison sampler or derivatives.

The retained link/state-tracker probe produces fragment NIR BLAKE3:

```text
df2b5db6 9b0dd382 32861295 bdb5ffb8
7ded4cc0 ef1ba4ff 62406d48 7d48f4ed
```

The complete dump is `/tmp/ioq-pshadow-compact-nir.txt`, SHA-256
`7c5017cb622597d7039b6a1a3577aa43872db1126bd221a2c7a2d6abd1c2af56`.
The current generic backend rejects it before producing semantic records.
The first missing operation classes are precise:

- one `fdot2(st, st)` used by `sqrt`;
- two scalar `fsign` operations used to mask forward- and normal-facing
  fragments;
- `fneu(sample.r, 1.0)`, its inverse, and `terminate_if`;
- the post-termination select `fneu ? intensity : 0`.

All other arithmetic is already in the bounded fragment vocabulary: ADD,
MUL, DP3, RCP, RSQ, MIN/MAX, source NEG, one computed-coordinate TEX, and a
single terminal COLOR result.  This makes pshadow a smaller next compiler
rung than shadow samplers, PCF, or derivatives.

## `fdot2` is a zero-padded DP3

The native DP3 record consumes three selected lanes.  A generic `fdot2(a,b)`
can therefore use the existing DP3 opcode with sources
`(a.x,a.y,ZERO,ZERO)` and `(b.x,b.y,ZERO,ZERO)`.  This preserves the exact
two-product sum and does not allocate a literal or an extra ALU record.

The lowering must be explicit.  Passing a two-component NIR source to the
current three-component source helper is invalid, and reading an undefined Z
lane would make the result dependent on unrelated packed data.  The offline
regression must assert both ZERO selectors and a one-record DP3 semantic
result.

## Finite-domain `fsign`

For a finite, bounded scalar `x`, exact GLSL/NIR sign can be expressed with
the already qualified ABS and CMP semantics:

```text
a = ABS(x)
s = CMP(x, +1, -1)       # x >= 0 ? +1 : -1
r = CMP(-a, 0, s)        # -abs(x) >= 0 ? 0 : s
```

This returns zero for both signed zeros and the correct sign for every finite
nonzero value.  Native source NEG is legal in the A slot of the second CMP;
it must not be placed in the unqualified C slot.  NIR defines `fsign(NaN)` as
zero, while the VX900 CMP NaN behavior is not qualified, so this lowering
must remain behind a draw-domain proof.  For pshadow that proof must cover the
varying and constant lanes feeding each dot and bound the products/sums so
they cannot form NaN through overflow cancellation.  Ordinary ioquake world
coordinates satisfy such a bound, but the compiler may not assume that from
the application name.

The host regression needs positive, negative, `+0`, `-0`, an out-of-bound
input rejection, and a source-placement check for both CMP records.  NaN
constant uploads remain rejected.

## Exact sampled-value equality discard

The source says:

```glsl
part = float(texture2D(u_ShadowMap, st).r != 1.0);
if (part <= 0.0)
    discard;
```

For a normalized sampler result `x`, the shader discards exactly when
`x == 1.0`.  A threshold approximation is wrong for filtered values close to
one.  The exact finite chain is:

```text
d = SUB(x, 1)
a = ABS(d)
k = CMP(-a, -1, +1)      # equality -> -1, inequality -> +1
KILL(k)
```

For every finite float distinct from one, `abs(x-1)` is positive; for one it
is zero, including either signed-zero subtraction result.  The normalized
sample domain excludes NaN.  Broadcast `k` to all four KILL lanes so no
undefined selector participates.  This uses the already recovered rule that
TEXKILL discards negative lanes.

After that KILL, every fragment reaching the output is on the true arm of
`bcsel(fneu, intensity, 0)`.  An exact dominance recognizer may replace the
select with `intensity`; a general bcsel or control-flow assumption is not
needed.  The recognizer must require the same comparison def to feed both the
terminate condition and select, no intervening side effect, one normalized
non-shadow 2D sample, and no other terminate/discard.

## Implementation and qualification order

1. Add isolated host tests for zero-padded DOT2 and the finite-domain SIGN
   expansion.  Reject NaN/unbounded draw domains rather than guessing CMP
   behavior.
2. Add an exact sampled-`!=1` terminate recognizer which emits
   SUB/ABS/CMP/KILL and removes only its dominated zero select.
3. Re-run the exact linked pshadow probe.  Record semantic ALU/TEX/KILL count,
   sampler/varying/constant masks, source pressure, scheduled pressure, and
   the kernel compiler's byte result.  Do not raise resource caps to make it
   fit.
4. First silicon gate: a guarded one-texture equality strip containing
   values below one and exactly one, with D16, exterior, full BO guards,
   immutable source and a known-good successor draw.
5. Only after that gate, submit the exact linked pshadow program with bounded
   finite varyings/constants and CPU-calculated interior samples.

Passing shader link, producing a semantic stream, or retiring a draw without
pixel checks is not sufficient.  The default object remains unsupported until
the equality KILL and full pshadow readback gates pass.

