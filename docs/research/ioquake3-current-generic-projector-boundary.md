# ioquake3 current generic projector boundary

Status: source and retained-live-capture audit on 2026-09-24.  No target draw
was issued for this audit.  The current Mesa and kernel production sources
were not changed.

## Evidence boundary

The first retained GL1 fragment rejection is exact, but it predates the
current closed compatibility classifiers.  It is in target artifact
`/root/tmp/chrome9/ioquake3-gl1-nir-c146218.log`, SHA-256
`ce03f8cc037663d5e36177ac8bba4b33744d3d2a970aa45e28eb6cc3efaae840`.
The fragment NIR is:

```text
q       = smooth TEX0.w
st      = smooth TEX0.xy
t       = tex2D(unit 0, coord=st, projector=q)
p       = COL0
product = t * p
out     = vec4(product.x, product.y, product.z, p.w)
```

It has one normalized 2D sample, one smooth full primary color, no constant,
one full color store, and no discard or flow.  The adjacent captured program
is the same projected sample followed by a full four-lane `t * p`.  The
ordered later fixed-function programs are retained in
`ioquake3-gl1-next-nir-v1.log`, SHA-256
`d83edcf99e2667b21c67de60299d817d268ad2fee8e2ac048a646ee834f14850`:

1. projected sample times primary RGBA;
2. projected sample times current-color RGBA;
3. projected sample times current-color RGB with current-color alpha;
4. two projected samples times primary, preserving primary alpha; and
5. the same two-sample RGB product with texture-zero alpha times primary
   alpha.

The latest archived application run is not current-HEAD compiler evidence.
`ioquake-safe-v2/gl1-q3dm1/stderr.log`, SHA-256
`15ae81d5b62a7f009fad2e29413a46d4f7edfeea422c156b9c69199483391510`,
used Mesa `d621583564`; its first reported boundary was the then-intentional
`textured-modulate-blend-unproven` state gate.  Subsequent closed projected,
blend, depth and lightmap commits mean that log cannot establish the first
rejection of current HEAD.  A new bounded application run is required before
calling any later state or shader the current live first failure.

## Current generic compiler rejection

The closed classifier recognizes all five shapes above by application-neutral
NIR structure.  The generic semantic compiler does not.  In current
`chrome9_context.c:3283..3298`, a 2D TEX is accepted only when every texture
source is the coordinate source.  The retained ioquake instruction has a
second `nir_tex_src_projector`, so semantic lowering returns false before it
can lower the color expression.

This is independent of the RGB/alpha reconstruction.  Once the projector is
removed from a private semantic clone, the current generic vector builder can
represent the first expression without a split color output:

```text
TEX   sample.xyzw = TEX(T0.xy)
MUL   product.xyz = sample.xyz * primary.xyz
MOV   result.xyz  = product.xyz
MOV   result.w    = primary.w
MOV   oC0.xyzw    = result.xyzw
```

The exact allocation and lifetime bits must come from the production compiler
selftest; the five semantic operations above are not a claim of finalized
native dwords.  They obey the kernel's current single terminal full-mask color
write rule.  Full RGBA modulation is smaller: one TEX and one terminal MUL.

The generic RGB/alpha form is more conforming than the old format shortcut.
It ignores sampled alpha because that is what the linked NIR says, so it is
correct for both XRGB and BGRA views.  Reusing a full-RGBA multiplication is
equivalent only when the translated sample supplies alpha one.

## Bounded identity-projector lowering

Every relevant ioquake3 GL1 renderer call supplies two-component texture
coordinates.  OpenGL fills the missing Q lane with exactly one, and existing
closed draws already require post-TNL Q raw bits `0x3f800000`.  That supports
a generic, application-independent lowering with these invariants:

1. On the private semantic NIR clone, accept only ordinary non-array,
   non-shadow 2D TEX whose coordinate is direct lanes XY of one interpolated
   vec4 and whose sole extra source is lane W of that same input.  Reject
   offsets, bias, explicit LOD/gradients, coordinate arithmetic, a different
   projector source, and every additional texture operand.
2. Record an identity-projector requirement against the logical varying; do
   not merely delete the source.  Remove the projector only in the private
   clone passed to the ordinary TEX lowerer.  The canonical NIR and variant
   key retain the original semantics.
3. The structured program may report producer width two because the GPU
   consumes only S/T after the proof.  Mesa's vbuf record still contains four
   float lanes, so draw-time validation can inspect Q before packing the
   kernel request.
4. Validate every post-TNL vertex in the complete Gallium draw before the
   first ioctl: S/T finite and Q bitwise one.  This must use the existing
   whole-draw deferred/preflight mechanism rather than rejecting one late
   vbuf callback after earlier triangles were submitted.  Failure falls back
   or rejects with zero published triangles.
5. Carry the requirement in every captured shader variant.  It is part of
   shader semantics, not sampler state, and must survive uniform specialization
   and variant-cache transitions.

This lowering needs no kernel opcode or TAU extension.  It selects the already
qualified ordinary TEX operation after proving that division by Q is an
identity.  General projective sampling is a different rung: proprietary
compilers encode TEXLDP operation seven (`207001b0` before scheduling), but
its full runtime linkage, implicit-LOD behavior and VX900 execution are not
live-qualified.

## Next GL1 compiler and state rungs

After the first RGB/primary-alpha form, implement in captured order rather
than by shader hashes:

1. full projected RGBA sample-times-primary, using the same identity-Q
   metadata;
2. projected sample times a direct full vec4 current-color uniform, including
   the RGB-only/current-alpha constructor;
3. two projected samples times primary with one identity-Q requirement per
   coordinate varying and exact independent output-alpha DAGs;
4. classic texture-environment ADD and missing-alpha REPLACE, where RGB and
   alpha are separate lane groups and RGB ADD is saturated; and
5. only then any nonunit-Q TEXLDP path.

For gameplay, shader admission is not enough.  Preserve the already separated
state gates: complete mip views and mip filtering for world textures;
BGRA/XRGB base-format alpha translation; alpha test combined with texturing
and writable LEQUAL depth for masked surfaces; then read-only EQUAL depth plus
ONE/ONE, DST_COLOR/ONE and source-over blends for dynamic-light and fog
passes.  These are generic GL states, not ioquake scene keys.

## GL2 startup after the old uniform error

The historical GL2 startup error reported too few default fragment-uniform
components when the driver exposed one vec4.  That resource limit is no
longer the current source boundary.  With kernel capability
`DRM_CHROME9_CAP_FRAGMENT_CONSTANTS_32`, current
`chrome9_screen.c:355..360` exposes 512 bytes, 32 vec4 or 128 components.
The largest audited default ioquake declaration is the 71-component
shadow-mask form, so this cap is sufficient for linking on component count.

Extended linkage now exposes eight fragment inputs and eight logical sampler
slots.  Default ioquake uses sampler indices through six, at most six
simultaneously, so the old sampler-assignment link boundary is also covered by
the current advertised resource count.  This still does not make default GL2
executable.  The first source-exact default lightall backend blocker is FACE
(`gl_FrontFacing`) selection, followed by SQRT lowering and physical temporary
pressure in the five-sampler lighting program.  A fresh startup/gameplay run
must confirm the new first live rejection after matching kernel/Mesa bits are
loaded.

