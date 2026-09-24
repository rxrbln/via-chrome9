# ioquake3 current generic projector boundary

Status: source, compiler and bounded live-capture audit on 2026-09-24.  The
identity-projector implementation was exercised by a target application run;
this document did not initiate a target draw or change kernel production
source.

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

An older archived application run is not current-HEAD compiler evidence.
`ioquake-safe-v2/gl1-q3dm1/stderr.log`, SHA-256
`15ae81d5b62a7f009fad2e29413a46d4f7edfeea422c156b9c69199483391510`,
used Mesa `d621583564`; its first reported boundary was the then-intentional
`textured-modulate-blend-unproven` state gate.  Subsequent closed projected,
blend, depth and lightmap commits mean that log cannot establish the first
rejection of current HEAD.

A newer pre-implementation bounded application run records the boundary that
the identity-projector change closes.  Target artifact
`/root/tmp/chrome9-live/ioquake-20260924/gl1-default-mip.log`, SHA-256
`270a6abc8d6fd39c143f286cd9cd666e2efb3d37957a5b88699a329ac77444ba`,
reports Mesa `git-a854407ab5`, enters `q3dm1`, loads cgame and all map
surfaces, and then rejects the first unsupported draw as:

```text
reason=textured-modulate-blend-unproven fs=16 layout=1 tex=1
views=1 samplers=1 target=281 depth=0 dsa=0/0/0
blend=1/0/3/19/0/3/19 mask=0xf blend_successor=1
```

Enum value 16 is `CHROME9_FS_TEXTURE_PROJECTED_MODULATE`.  The same log
submits two ordinary `textured+d16` triangles immediately before the reject.
This proves that the first retained GL1 application stop before the new
lowering was the closed projected-MODULATE program combined with blend, not
mip allocation or basic textured depth.  It does not by itself prove the
later world-material passes.

## Implemented generic identity-projector path

The generic semantic compiler now recognizes the identity-projector case by
application-neutral NIR structure.  The accepted shape is an ordinary 2D TEX
with direct XY coordinate lanes and a projector from W of the same fragment
input.  It records an identity-projector varying mask, removes the projector
only from the private semantic clone, and passes the resulting ordinary TEX
through the existing instruction builder.  The draw path preflights the
complete post-TNL draw before publishing its deferred command-22 batches and
requires the corresponding Q lane to have raw bits `0x3f800000`.  A mismatched
projector, including the two-projector regression's T1.z negative case, stays
fail-closed.

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

The compiler regressions cover one projector and two independent projectors
(identity masks `0x08` and `0x88` respectively), including the RGB/alpha DAGs
above.  The exact allocation and lifetime bits still come from the production
compiler rather than from the illustrative operations here.  They obey the
kernel's current single terminal full-mask color-write rule.  Full RGBA
modulation is smaller: one TEX and one terminal MUL.

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

## Capped q3dm1 hardware evidence

The corrected TEX0-dereference build is preserved as target
`libgallium-26.2.2.so`, SHA-256
`c6916db5cbdf4f613e8db2d52cd442b3ce9ad35d3a1fe500bf7ca282d8d0830c`.
Two bounded q3dm1 captures use that build:

- `gl1-mip-limit1-tex0.log`, SHA-256
  `c20c4c101083071c8777db9224ec23a9893ee0c06150ad8587237b3665d32443`,
  stops at the next triangle with the generic counter equal to one; and
- `gl1-mip-limit64.log`, SHA-256
  `74d43c884eb92d29d6fa429e06083808546ae1df63bceb9f48530d8cc872a98f`,
  stops at the next triangle with the counter equal to 64.

The draw-limit check includes submitted, deferred and current generic batch
triangles.  The guarded-domain end publishes a deferred batch only after the
whole-draw identity-Q proof succeeds.  Thus the second capture is hardware
submission/retirement evidence for 64 generic command-22 triangles, not only
shader admission.  The first executed class is program 18, four ALU plus one
TEX and constant c0.  Before the limit line, programs 19, 16, 17 and 18 all
report `supported=1`; programs 24, 25 and 9 are created later and also report
supported, but the cap prevents treating those later bindings as hardware
execution evidence.  The projected classes have 5/1, 2/1, 2/1, 5/1, 7/2 and
7/2 total/TEX records as applicable.  There is no unsupported,
bounded-state-reject, ioctl-failure, timeout, quarantine or GPU-fault line in
the application captures or the contemporaneous kernel log.  The userspace
trace does not retain a numeric ioctl cookie; command kind 22 follows from
the source path `batch_generic_program -> chrome9_draw_fragment_program`.

This is not a pixel oracle.  It proves that the exact application-side
projector classes survive compiler, whole-draw guard, generic state builder,
kernel validation and GPU retirement for the bounded prefix.  It does not
prove the RGB/alpha equation, blend result, mip choice or depth result.

## Next GL1 compiler and state rungs

The bounded compiler trace now admits the captured one- and two-projector
forms in NIR-structure order rather than by shader hashes: full projected
RGBA sample-times-primary; projected sample times a direct current-color
uniform, including the RGB-only/current-alpha constructor; and two projected
samples times primary with independent identity-Q requirements and output
alpha DAGs.  Programs 24 and 25 establish compiler admission for the latter
two-texture forms, not executed-pixel correctness because they are created
after the 64-triangle stop.

Remaining GL1 compiler rungs are classic texture-environment ADD and
missing-alpha REPLACE, where RGB and alpha are separate lane groups and RGB
ADD is saturated, followed only later by any nonunit-Q TEXLDP path.

For gameplay, shader admission is not enough.  Preserve the already separated
state gates: complete mip views and mip filtering for world textures;
BGRA/XRGB base-format alpha translation; alpha test combined with texturing
and writable LEQUAL depth for masked surfaces; then read-only EQUAL depth plus
ONE/ONE, DST_COLOR/ONE and source-over blends for dynamic-light and fog
passes.  These are generic GL states, not ioquake scene keys.

## Projected-MODULATE plus blend qualification

The projector lowering and capped run make the retained first draw
representable and prove bounded command retirement, but they do not qualify
the pixel result.  There is no retained pixel oracle for generic command 22
combining TEX times primary color with ioquake's same-factor
`SRC_ALPHA,INV_SRC_ALPHA` blend.  Existing Gallium source-over results either
use REPLACE or the canonical separate-alpha equation, while the older fixed
MODULATE list has one completion-timeout incident.  Vendor state builders do
strongly support the packet shape:
A34--A37 and the RT A00 blend gate are built independently of the pixel
shader and TEX count, and the generic request carries the same normalized
blend block beside the semantic program.

The smallest safe discriminator is one no-depth triangle, not an application
run:

- guarded 32x32 BGRX target cleared green;
- guarded 2x2 BGRA texture, nearest/clamp L0, texel `80ff0000`;
- white primary, direct T0.xy, and Q raw bits exactly `3f800000`;
- the exact projected full-RGBA texture-times-primary program; and
- `SRC_ALPHA,INV_SRC_ALPHA` for both RGB and alpha.

The covered BGRX pixel must be approximately `00807f00` in RGB, with one LSB
tolerance.  Require one generic program batch, exact retirement, unchanged
texture/exterior/guards, and a generic no-blend TEX baseline before and after.
Stop on the first timeout.  If this closes, repeat only once with a BGRA
target cleared to transparent green.  The exact ioquake same-factor equation
stores approximately `40807f00`: alpha is `As*As = 0.25`.  The canonical
separate-alpha source-over result `80807f00` is a negative discriminator.
This second draw also proves A52 format 99 rather than merely RGB agreement on
an XRGB target.

An existing ordinary `--texture-only` MODULATE smoke is not a substitute for
that gate.  Target `texture-only-debug.log`, SHA-256
`23805d1a9cc17d7807886bc54c6c51b8484378a9a1add90a23fa10233b5554b3`,
reports program 15 as semantic/native-valid but observes zero submitted and
zero rejected triangles (`+0/+0`) and four zero samples instead of the red
pattern.  That is a separate unresolved draw-path/control result, not evidence
that the q3dm1 command-22 pixels are correct.

After a numeric pixel gate, source and current capability review find no
deterministic generic-state rejection for the next q3dm1 material classes.
The first
representative two-pass material is protobanner: projected BGRA times primary
with GE128 and LEQUAL/write, followed by projected XRGB lightmap times white
with `DST_COLOR,ZERO` and EQUAL/read-only.  Dynamic light uses XRGB times
primary, clamp/linear L0, `ONE,ONE` or `DST_COLOR,ONE`, EQUAL/read-only.  Fog
uses BGRA times primary, clamp/linear L0, same-factor source-over, and
LEQUAL/read-only or EQUAL/read-only.  A bounded current application run is
therefore the correct next discovery step once the single-triangle gate
passes; no later shader should be called the current first failure before
that run.

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
