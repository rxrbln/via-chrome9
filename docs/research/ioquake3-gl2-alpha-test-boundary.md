# ioquake3 GL2 lightall alpha-test boundary

Status: source and current-backend audit, 2026-09-24.  Mode zero is closed by
direct-UBO terminate-leaf specialization.  Modes one through three are not
yet generic lightall support and have not been submitted on VX900.

## Exact application predicates

`lightall_fp.glsl:296-312` samples the diffuse map and defines

```glsl
float alpha = diffuse.a * var_Color.a;
```

It then selects one of three discard predicates:

| `u_AlphaTest` | discard predicate |
|---|---|
| 1 | `alpha == 0.0` |
| 2 | `alpha >= 0.5` |
| 3 | `alpha < 0.5` |

`tr_shade.c:1155-1170` maps the Quake stage bits `GT_0`, `LT_80`, and
`GE_80` to modes one, two, and three respectively.  Ordinary stages set mode
zero.  Mesa commit `04a35c3` keys the direct CB0 mode leaf; its exact-shape
test proves mode zero removes all three `terminate_if` intrinsics and each
nonzero mode retains exactly one.

## Why the current KILL matcher is intentionally narrower

The current generic matcher in `chrome9_context.c` accepts only
`terminate_if(flt(tex2D(s0).a, 0.5))`.  It emits the silicon-qualified
replicated `sampled.a - 0.5`, then ordered KILL.  Finite KILL rejects a
negative component, so equality survives.  This contract has guarded color
and D16 evidence.

Lightall's predicate is not that shape: its alpha is a multiplication of a
sample and an interpolated color.  Shader NIR alone does not prove either
operand finite and in `[0,1]`; the sampled image format and the post-VS
varying domain are draw state.  Recognizing the application expression while
omitting those proofs would widen discard semantics to NaN, infinity, and
unbounded user float inputs.

## Implementation order

Mode three is the smallest general extension.  With draw metadata proving
both operands finite and in `[0,1]`, normal ALU lowering can produce the
multiply, replicate `alpha - 0.5`, and issue the already-qualified KILL.  The
proof must cover the bound sampler format and all post-VS color-alpha values
before the first emitted triangle; shader-source identity is not a proof.

Mode two cannot simply reverse the subtraction.  `0.5 - alpha` is negative
only for `alpha > 0.5`, while GLSL requires equality to discard too.  The
qualified construction is:

```text
condition = alpha - 0.5
signed_kill = CMP(condition, -1, +1)  # -1 when condition >= 0
KILL signed_kill
```

VX900 has already qualified TEMP-backed native CMP for negative, positive,
`+0`, and `-0`, including the nonnegative zero arm.  Literal packing must put
the two arms in one physical constant vector (or materialize one arm in a
TEMP) so the known three-distinct-register source-port limit is not violated.
The same finite `[0,1]` draw proof is still required.

Mode one remains fail closed.  A sign-only KILL has no conforming transform
for `alpha == 0` over an arbitrary finite float domain: subtracting an epsilon
also discards small positive values, and the hardware RCP/FTZ boundary is not
an equality predicate.  A later implementation needs either a qualified
native equality sequence plus KILL or a stronger, generic quantized-domain
proof.  It must not special-case ioquake material names.

## Gate

Default lightall should first run with mode zero.  For mode three, use a
guarded diffuse-alpha times varying-alpha oracle with below, equal, and above
half products, D16, and an accepted farther draw proving killed fragments
write neither color nor depth.  Mode two needs the same values with the
opposite expected boundary.  Do not enable mode one from those results.

## Evidence

- ioquake source tree:
  `/srv/t2/intel-poulsbo/ioq3-93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`.
- KILL packet and live contract:
  `chrome9-texkill-schedule-audit.md`.
- CMP zero-boundary and source-port contract:
  `chrome9-cmp-abs-sat-rung.md`.
- Mode-zero specialization: Mesa commit `04a35c3`.
