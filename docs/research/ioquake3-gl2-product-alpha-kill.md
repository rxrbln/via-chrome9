# ioquake3 GL2 product-alpha KILL boundary

Status: exact linked-NIR capture, guarded Mesa implementation, bounded q3dm1
compile/bind qualification, and guarded product-KILL silicon proof,
2026-09-24.  A later game run must still reach a mode-three draw.

## First post-startup rejection

Mesa commit `bc9e879` removes the earlier vertex `ishl` assertion by lowering
the bounded bone UBO address scale to float arithmetic before `nir_to_tgsi`.
A cap-one q3dm1 run then reached its first normal fragment draw rejection.
`CHROME9_DUMP_UNSUPPORTED_NIR=1` identifies it as `GLSL172`, source BLAKE3:

```text
2755992e 629ee07a e600634e 1d03e8cf
17975a35 7cec7c94 a7e5b4fe 380650c8
```

The draw-time fragment variant is `face=0`, uniform dword zero
`0x40400000` (`3.0f`), matching ioquake's alpha-test mode three.  The exact
specialized operation is:

```text
sample = tex2D(s0, VAR0.xy)
alpha  = sample.w * VAR1.y
terminate_if(alpha < 0.5)
color  = vec4(sample.xyz * vec3(VAR0.z, VAR0.w, VAR1.x), alpha)
```

There is no FACE read in this NIR.  The previous direct-alpha KILL lowerer
accepted only `tex2D(s0).w < 0.5`, so the product is the deterministic miss.
The capture is on the target at:

```text
/root/tmp/chrome9-live/ioquake-gl2-boneshift-20260924/
    gl2-limit1-unsupported-nir.log
```

The bounded process exited, `/dev/dri/renderD128` remained present, and the
immediate dmesg capture contained no new Chrome9 timeout or engine fault.

## Generic admissible domain

The native KILL sequence discards a negative component after subtracting
`0.5`.  It implements the ordered finite mode-three predicate exactly for:

```text
normalized_sample_alpha * smooth_varying_factor
```

only when both operands remain in `[0, 1]`.  The generic sampler path exposes
only explicitly enumerated UNORM formats; nearest, linear and mip filtering
and their canonical zero/one view swizzles preserve that interval.  Mesa also
records the direct varying lane used by the product.  Before submitting any
triangle, the draw module validates every post-VS vertex in every vbuf
callback as finite and within `[0, 1]`.  Positive finite-normal RHW and
perspective interpolation preserve the interval.  All guarded batches remain
deferred until the complete Gallium draw has passed, and the queue path repeats
the per-triangle check as a defense.

This is a data-domain proof, not an ioquake shader key.  The compiler rejects
a transformed/non-direct factor, a non-alpha sampled lane, the reversed
predicate, a threshold other than exact `0.5`, a non-2D/nonzero sampler, and a
context without fragment KILL capability.  Runtime rejects a future float or
signed-normalized sampler format even if it is later added to the generic
format table.

The source-exact host regression expects varying mask `0x3`, sampler mask
`0x1`, and unit-interval lane mask bit five (`VAR1.y`).  Its semantic object
contains one TEX, two MULs, one SUB and one KILL before the terminal full-color
write.  Range tests accept signed zero, positive subnormal, `0.5`, and `1.0`,
and reject negative values, values above one, infinities, and NaN.

## Bounded q3dm1 qualification

The independently reviewed library with SHA-256
`c962f89b465bec83c0a6da55b4e4a11b2748853b356c647acfd6edd5b99ba3a7`
ran q3dm1 under the exclusive VX900 lock with `CHROME9_DRAW_LIMIT=1`.  Startup
completed.  All 16 logged draw-time fragment variants reported `supported=1`;
there were zero rejected variants, zero `unsupported specialized FS` messages,
and zero draw rejections.  In particular, the exact mode-three variant was
created and bound twice as:

```text
chrome9: FS variant 2 face=0 dw0=40400000 supported=1 ir=10 alu=0 tau=0
```

The run stopped before triangle one as requested and was killed by the outer
20-second bound (`RUN_RC=137`).  `/dev/dri/renderD128` remained present and the
immediate dmesg capture had no new Chrome9 timeout or engine fault.  Evidence
is preserved on the target at:

```text
/root/tmp/chrome9-live/ioquake-gl2-mode3-20260924/
    gl2-limit1-fixed.log
    health-after-fixed.log
```

This proves specialization, semantic compilation, runtime-domain acceptance,
and binding of the exact q3dm1 product form.  Because the one-triangle cap hit
an earlier shader, it does not itself prove execution of this KILL program on
silicon.

## Guarded silicon discriminator

Tools commit `d3da57a` adds a raw cmd22 oracle whose five-record semantic
stream is TEX, scalar MUL, replicated SUB, identity KILL, and terminal MOV.
Two rectangles sample the same normalized BGRA texel with alpha `0xbf`
(about `0.749`).  One supplies varying factor `0.5`, so the product is about
`0.375` and must be discarded; the other supplies `0.75`, so the product is
about `0.562` and must survive.  The pair distinguishes the product from
testing either operand alone: sample alpha alone survives both, while the
factor-only equality case would not be negative.

The independently reviewed binary with SHA-256
`295f92a241dfce6ca81f85e470c3d77780fc827a94c7d0ea5ba2b18593cd736a`
passed once on VX900 under the exclusive GPU lock:

```text
product-alpha KILL passed: .749*.5 discarded, .749*.75 survived;
D16, guards, and source intact
```

All sampled killed pixels retained clear color and D16, all surviving pixels
contained the exact source BGRA and expected D16, exterior pixels stayed
clear, color/depth/texture guard pages and immutable user inputs remained
intact, and the render node remained present.  The immediate new-dmesg capture
was empty.  Evidence is preserved at:

```text
/root/tmp/chrome9-live/ioquake-gl2-mode3-20260924/
    product-alpha-kill.log
    product-alpha-kill-health.log
```

The native product-alpha KILL path is therefore silicon-qualified in the
bounded normalized domain.  The remaining application gate is a controlled
GL2 run whose cap reaches a mode-three draw and reports it as the active
submitted variant.  Alpha modes one and two retain their separate equality
and opposite-boundary requirements.
