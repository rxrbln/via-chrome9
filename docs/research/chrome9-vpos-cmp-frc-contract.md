# Chrome9 vPos, CMP, and FRC compiler/runtime contract

Status: production Linux compiler objects are byte-exact; Mesa state-tracker
integration is source-exact; native `vPos`, CMP, and FRC numerical behavior
still requires the guarded silicon oracles below.  This note does not promote
an API capability by itself.

## What the vendor objects establish

The Linux 5.75 pixel compiler treats D3D PS 2.0 `vPos` as a
`D3DSPR_MISCTYPE` source.  It sets the compiled-object `+0x70` predicate,
increments the first resource count before choosing a resource bucket, and
replaces raw source id `0xcf` with physical id 3, 4, 7, or 15.  Final use of
that id contributes a usage bit to A90, or A91 bit 23 for id 15.  No ordinary
texture-coordinate input is allocated for `vPos`.

The smallest finalized objects are:

```text
MOV oC0,vPos.xyxy
A90..A93 90058008 91000000 92000001 93000000
ALU       01c00530 101c0053 00f2c304 000d0820

FRC oC0,vPos.xyxy
A90..A93 90058008 91000000 92000001 93000000
ALU       01c00530 101c0053 00f2c304 000e0820

FRC oC0,vPos
A90..A93 90058008 91000000 92000001 93000000
ALU       01c00530 301c0053 00f2c305 000e0820

MUL r0,vPos,c0; FRC oC0,r0
A90..A93 90058008 91000000 92000002 93000000
ALU       0140b6d0 30080053 00f2c305 00000020
          01c00530 301c0053 00f20005 000e0820
```

The XY-only source token is `10441000`: D3D `MISCTYPE`, register index zero,
swizzle XYXY.  Both minimal rows report resource tuple `(0,0,0,0)`, have no
TAU or ordinary varying, and differ only in ALU subop (`d` MOV versus `e`
FRC).  Configuration two relocates the raw MISC sentinel `0xcf` to native
class 3/id 3; finalized dword two `00f2c304` contains that id, class and
terminal-use marker, while dword one `101c0053` carries XYXY.  A narrow
semantic ABI can therefore append POSITION as source-file value 3, require
index zero and XY-only selectors, and keep A00/A08/FVF varying counts zero.
Initially reject POSITION modifiers, Z/W selectors and unused operands rather
than treating native class 3/id 3 as an ordinary T3 input.

The exact two-record pixel-center object is:

```text
# r0.xy = vPos.xyxy * c0.xyxy; FRC oC0,r0.xyxy
resources 0 1 0 0
A90..A93 90058008 91000000 92000002 93000000
ALU       0140b6d0 d008006d 0032c306 00000020
          01c00530 101c0053 00f20004 000e0820
```

Its tokens are retained in the corpus.  With c0.xy exactly 0.25, native
half-integer centers produce fractional XY values
`.125,.375,.625,.875` across a four-pixel axis, replicated as XYXY.  The
object still has A00 resource count zero, no TAU and no ordinary varying;
only TEMP0 lifetime joins the already closed MISC relocation.

The second object's unfinalized MISC source is `0xcf`; configuration two
rewrites it to 3 and marks the consumer's temporary source as final.  FRC is
an ordinary one-A-source ALU with subop `e`; it needs no scheduler sideband.

CMP is a three-source ALU with subop 9 and vendor operand ordering:

```text
A = condition, C = true arm, B = false arm
```

For `SUB r0,c0,c1; CMP oC0,r0,c2,c3`, configuration two produces:

```text
A90..A93 90058000 91000000 92000002 93000000
80800530 30140924 00f08105 00000020
00830530 30082053 00f20005 00090820
```

The complete glmark condition core compiles as nine ALUs and eight physical
temporaries.  Because the MISC source then relocates to id 15, its terminal
usage appears in A91 bit 23:

```text
A90..A93 90040000 91800000 92000009 93000000
```

All tokens and instruction records are retained in
[`chrome9-vpos-cmp-frc-corpus.tsv`](chrome9-vpos-cmp-frc-corpus.tsv).  The
corpus is a compiler/finalizer oracle, not proof that silicon implements the
expected arithmetic.

## Raster linkage boundary

The static evidence supports a narrow model: the pixel engine supplies one
special MISC vector selected by the compiled program's A90/A91 resource
usage.  The pixel compiler/finalizer emits no FVF mutation, and the inspected
ordinary raster FVF paths program position, colors, fog, and texture-coordinate
lanes without a recovered dependency on the pixel object's MISC predicate.
Therefore the initial implementation must keep `vPos` outside the T0/T1
allocation and must not fabricate it as a vertex varying.  A live oracle is
still required because absence of a recovered hidden FVF branch is not proof
of hardware behavior.

There is still an important ambiguity.  `mov oC0,vPos` and
`mov oC0,vFace` compile byte-for-byte identically, including metadata and
controls.  No recovered object field distinguishes the two D3D source
register numbers.  Admit only `vPos.xy` after a live position oracle; reject
`vFace` until a separate front-face mechanism is found and tested.

D3D `vPos` has only X and Y.  Mesa's SVGA D3D9 backend independently has to
reconstruct fragment Z and reciprocal W through another interpolator because
native D3D `vPos` does not contain them.  The Chrome9 hardware path must thus
reject `gl_FragCoord.zw` or use a separately proven generated-varying
reconstruction.  Enabling XY for the present glmark programs does not prove
full four-component `gl_FragCoord`.

## Mesa representation and Y transform

Chrome9 currently advertises only:

```text
fs_coord_origin_upper_left = true
fs_coord_pixel_center_half_integer = true
```

It leaves `fs_position_is_sysval` false.  That makes GLSL `gl_FragCoord` a
fragment POSITION input rather than a system value, even though native vPos
is not an interpolated VS output.  Once the silicon position oracle passes,
the correct representation is:

```text
fs_position_is_sysval = true
```

and the backend must recognize `load_frag_coord` / `load_frag_coord_xy` as
the MISC vPos source.  It must not add POSITION to the ordinary varying map.

Mesa's `nir_lower_wpos_ytransform` already handles the difference between
the native upper-left origin and default GLSL lower-left coordinates.  For
the native upper-left-only claim, default GLSL chooses the XY pair of the
hidden state vector `STATE_FB_WPOS_Y_TRANSFORM`:

| draw-buffer orientation | state vector | resulting GLSL Y |
| --- | --- | --- |
| `FlipY=false` (ordinary FBO convention) | `(1,0,-1,height)` | `native_y` |
| `FlipY=true` (window-system drawable convention) | `(-1,height,1,0)` | `height-native_y` |

The hidden state parameter is dirty on `_NEW_BUFFERS`.  Treat it as an
ordinary driver constant and compile the generated weak FFMA as native MAD,
or as MUL+ADD if MAD is not yet admitted.  Never bake the current height or
orientation into a shader object without including them in its variant key.
Changing a drawable, framebuffer, or its height must update the constant
before the next draw.

There is no pixel-center bias for the current half-integer claim.  That claim
also remains conditional on live vPos producing `(x+0.5,y+0.5)` at single
sample.  The CPU draw path's `half_pixel_center` vertex-edge bias does not
prove what the pixel engine exposes through MISC.

## Generic FRC rules

NIR `ffract(x)` is `x-floor(x)`.  The D3D FRC token maps directly to native
subop e, supports ordinary source swizzles and destination masks, and
participates in the normal temporary last-use pass.  The first safe backend
rule is:

```text
finite FP32 ffract(source) -> native FRC
```

Do not apply a truncate-based rewrite.  Do not admit NaN, infinity, denormal
preservation modes, or sign-sensitive consumers until their silicon rows
pass.  For finite values the mathematical result is `[0,1)`, while an FP32
implementation is allowed to round a small negative fraction to 1.0; range
reasoning must therefore use `[0,1]`.

The installed glmark conditionals/function/loop fragment prefix is exactly:

```glsl
vec2 FragCoord = gl_FragCoord.xy;
float d = fract(FragCoord.x * FragCoord.y * 0.0001);
```

At the current maximum 2048-square target this stays finite and small before
FRC.  Each repeated glmark step is `d=fract(3*d)`, so its input remains in
`[0,3]`.  This is a generic finite-range proof derived from the IR and render
target limits, not a shader-name exception.

## Generic comparison/select rules

The vendor D3D contract for CMP is `condition >= 0 ? true : false`.  Static
encoding proves operand order, but the native subop's equality, signed-zero,
and unordered behavior still need silicon proof.  After nonnegative,
negative, `+0`, and `-0` rows pass, ordered finite NIR comparisons can lower
as follows:

| NIR predicate/select | condition | native true arm | native false arm |
| --- | --- | --- | --- |
| `bcsel(fge(x,y), t, f)` | `x-y` | `t` | `f` |
| `bcsel(flt(x,y), t, f)` | `x-y` | `f` | `t` |
| `bcsel(fle(x,y), t, f)` | `y-x` | `t` | `f` |
| `bcsel(fgt(x,y), t, f)` | `y-x` | `f` | `t` |

Require range analysis to prove x, y, their subtraction, and both selected
arms finite.  GLSL ordered comparisons are false for NaN; native CMP's
unordered rule is unknown, so merely proving the inputs non-NaN is essential.
Do not materialize a NIR boolean as float and do not infer arbitrary integer
`bcsel` support from this rule.

Finite equality needs two ordered tests, not one CMP.  One valid straight-line
shape is:

```text
d = x-y
inner = CMP(-d, equal_arm, unequal_arm)  # selects equal only when d <= 0
result = CMP(d, inner, unequal_arm)      # and d >= 0
```

This is correct only after both CMP zero boundaries and NEG source handling
are live-proven.  NaN still requires fallback.  `fne` swaps the arms.  This
generic expansion is relevant to the ideas lighting shaders; it is not needed
for the glmark conditionals scene's single `fge(d,0.5)`.

The conditionals fragment shader can therefore be if-converted without a
native branch:

```text
d     = FRC(vPos.x * vPos.y * 0.0001)
cond  = d - 0.5
t     = FRC(2.0 * d)
f     = FRC(3.0 * d)
out   = CMP(cond, t, f)
```

The finalized nine-record object in the corpus proves the resource and
dependency shape.  The compiler must still assemble RGB=`out`, A=1 through
generic vector construction; the corpus's replicated terminal scalar is not
permission to hard-code a scene output.

## Functions and loops

GLSL functions must be inlined in NIR.  Chrome9 has no proven pixel CALL
instruction.  The installed function shader then becomes the same vPos/MUL/
FRC graph plus the selected `process()` arithmetic; the five-record
`function_two_frc` corpus demonstrates two dependent FRC stages with ordinary
SSA temporaries.

The installed loop template substitutes `$NLOOPS$` into the loop body.  With
the scene's default `fragment-uniform=true`, that replacement is the
`FragmentLoops` uniform; with `fragment-uniform=false`, it can be the literal
`fragment-steps` value.  Any loop whose trip count is constant after GLSL/NIR
specialization must be fully unrolled before scheduling.  A linked form which
still depends on `FragmentLoops` needs a bounded native variant keyed on the
exact uploaded integer and unrolled to that value.  Refuse negative counts,
integer overflow, excessive ALU count, or excessive temporary pressure; use
the driver's real software fallback when the native 96-ALU/12-temp envelope
is exceeded.  Never truncate the iteration count.

## Guarded live oracles

Use a guarded BGRA8888 target, fresh passing D16, exact command capture,
ordered completion cookie, reset/error counters, and known-good draws before
and after every candidate.

1. **MISC identity.** Submit terminal MOV vPos and FRC vPos objects first.
   Require retirement and unchanged guard pages.  A vFace-labelled negative
   control must not be used to claim face support because its object is
   identical.
2. **Pixel centers.** On a 4x4 target render
   `FRC(vPos.xxxx*0.25)` and `FRC(vPos.yyyy*0.25)` in separate draws.  Expected
   UNORM values for half-integer coordinates are approximately
   `32,96,159/160,223`.  X must vary across columns and native Y across rows.
3. **Origin and offset.** Repeat inside a 4x4 viewport/scissor at nonzero X/Y
   in an 8x8 target with scale 0.125.  The values must include absolute target
   coordinates, not viewport-relative coordinates.  Then test Mesa's
   `STATE_FB_WPOS_Y_TRANSFORM` for both window and FBO orientations.
4. **FRC numerics.** Use exact vectors
   `(.25,1,-.25,-1)->(.25,0,.75,0)` and
   `(.5,.75,-.5,-.75)->(.5,.75,.5,.25)`, followed by the 2^23 precision
   boundary documented in `chrome9-frc-static-finalizer-audit.md`.
5. **CMP boundary.** Use condition lanes `(-.5,+.25,+0,-0)` with asymmetric
   arms.  Require negative to choose false and positive, +0, and -0 to choose
   true.  Bracket with reversed-arm controls.  NaN remains a separate gate.
6. **Composed graph.** Run the exact three-, five-, and nine-record corpus
   objects before exposing the lowering.  Compare reference pixels against
   llvmpipe for the installed conditionals/function/loop shader sources.

Only after rows 1--3 pass should Mesa set `fs_position_is_sysval`.  Only after
rows 4--6 pass should native FRC/CMP graphs replace fallback for finite IR.

## Evidence identity

The offline objects were generated with
`tools/re/chrome9_pps_probe.c` SHA-256
`4299ac3cb182c9e26688dd40e63e9aed4fb86ce47c829717b2663b734f040c13`
under `CHROME9_POSTPROCESS=2 CHROME9_RESOURCE_COUNTS=1`.  The Linux vendor DRI
binary SHA-256 is
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
The Mesa 26.2 `prog_statevars.c` and `nir_lower_wpos_ytransform.c` inputs have
SHA-256 values
`3c5d09c2d5f5a0747ea77e3c413a2f5661e54d2787bd4183a61c602001537643`
and `00ca2de448f854f465fbdda1788b469110d0a66fcec4f008c00d56392d335416`.
