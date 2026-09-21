# ioquake3 live lightmap and masked-surface successors

Status: live NIR capture, renderer/source audit, Linux/Windows compiler
differentials, and current command-12/13 packet audit, 2026-09-21.  The
three-factor fragment object is compiler-exact but has not yet run on VX900.
The alpha-test registers and state-complete one-/two-texture packets are
independently live; this note does not claim that their combination with the
new three-factor object has passed silicon.

## Live order and exact linked programs

The target log
`/root/tmp/chrome9/ioquake3-gl1-next-nir-v1.log` reaches the following fixed
function programs in order after loading q3dm1:

1. projected texture times primary RGB with primary alpha;
2. projected texture times primary RGBA;
3. projected texture times current-color RGBA;
4. projected texture times current-color RGB with current-color alpha;
5. two projected textures times primary, with neither texture contributing
   API alpha; and
6. two projected textures times primary, with texture 0 contributing API
   alpha and texture 1 not contributing it.

The fourth program was the first unsupported shader in that capture.  Later
captures admitted the one-texture state far enough to submit 1499 textured
blend triangles before reaching a new unsupported program.  Therefore the
two lightmap forms below are the exact next multi-texture classifiers, not a
source-only prediction.

### The exact q3dm1 masked material after lightmaps

The shipped q3dm1 BSP texture lump contains
`textures/base_wall/protobanner`.  Its exact `scripts/base_wall.shader`
definition is a two-pass material:

```text
{
    map textures/base_wall/protobanner.tga
    alphaFunc GE128
    depthWrite
    rgbGen vertex
}
{
    map $lightmap
    rgbGen identity
    blendFunc filter
    depthFunc equal
}
```

The surface is two-sided and vertex-deformed, but those operations are
resolved before the post-TNL immediate record.  The fragment/state sequence
is exact: projected BGRA texture times primary color, GE128, LEQUAL/write;
then projected XRGB lightmap times white, `DST_COLOR,ZERO`, EQUAL/read-only.
The BSP also names `models/mapobjects/gratelamp/gratetorch2b`, whose only
stage is projected texture times vertex color with GE128 and depth write.

The q3dm1 BSP effects/fog lump has length zero.  Dynamic lights may still be
introduced at runtime and `RB_FogPass()` remains needed by other maps, but
neither should displace the banner's alpha-test then EQUAL/filter pair as the
first q3dm1 state oracle after the general lightmap program.

The first live two-texture form is:

```text
t0 = tex(unit=0, coord=TEX0.xy, projector=TEX0.w)
t1 = tex(unit=1, coord=TEX1.xy, projector=TEX1.w)
p  = COL0
m0 = t0 * p
s0 = vec4(m0.rgb, p.a)
m1 = t1 * s0
out = vec4(m1.rgb, p.a)
```

Its exact output is `RGB=t0.rgb*t1.rgb*p.rgb, A=p.a`.  Both sample views have
the GL base-format alpha value one.

The second live form is:

```text
t0 = tex(unit=0, coord=TEX0.xy, projector=TEX0.w)
t1 = tex(unit=1, coord=TEX1.xy, projector=TEX1.w)
p  = COL0
m0 = t0 * p
m1 = t1 * m0
out = vec4(m1.rgb, m0.a)
```

Its exact output is `RGB=t0.rgb*t1.rgb*p.rgb, A=t0.a*p.a`.  Texture 0 has API
alpha and texture 1 has base-format alpha one.  These forms occur at lines
357--453 of the target log.  The earlier one-texture analogues occur at
lines 95--174 and independently establish Mesa's same base-format-alpha
normalization.

## One native program covers all four base-format-alpha variants

Let bit 0 of `alpha_mask` mean texture 0 supplies API alpha and bit 1 mean
texture 1 supplies API alpha.  Compatibility MODULATE requires:

| `alpha_mask` | exact fragment alpha | evidence |
|---:|---|---|
| `0` | `p.a` | live NIR |
| `1` | `t0.a*p.a` | live NIR |
| `2` | `t1.a*p.a` | stage-local fixed-function symmetry |
| `3` | `t0.a*t1.a*p.a` | full RGBA MODULATE |

Chrome9 image format 98 supplies sampled alpha one, while format 99 supplies
stored alpha.  Consequently the full-vector operation

```text
oC0 = t0.rgba * t1.rgba * p.rgba
```

is exact for every row when each view is translated according to its API
alpha semantics.  No per-alpha-mask native program or UAPI selector is
needed.  The important validation is on the translated sampler view, not on
the allocation's storage label:

* a logical RGB/XRGB view must use A30 format 98 and alpha swizzle ONE;
* a logical RGBA/BGRA view must use A30 format 99 and identity alpha; and
* a BGRA allocation reinterpreted by a legal XRGB view may use format 98,
  but format 99 plus an ignored software swizzle is not equivalent.

This also closes the existing projected one-texture variants.  Full native
`texture*primary` is exact for `vec4((texture*primary).rgb,primary.a)` only
when the translated sample format supplies alpha one; it is exact without
that restriction for the linked full-RGBA multiply.

The three-factor object remains:

```text
A90..A9a 90000000 91000001 92020003 93000002
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       a01001b0 311001b1 cccccccc dddddddd
ALU       0140b6d0 3003f000 00f20005 000003e0
          0140b6d0 00040053 00f03f00 200003e0
          01c00530 001c0053 00f03f00 000d0820
          cccccccc dddddddd
```

The Xorg scheduled form in
[`two-texture-primary-uapi-design.md`](two-texture-primary-uapi-design.md)
is the equivalent executable packet source.  The ordinary two-TAU hash is
`503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
Both post-TNL Q lanes must still be bitwise `0x3f800000`; the projected NIR
is lowered to ordinary TEXLD only under that identity-projector proof.

### Exact partial-alpha compiler differentials

The proprietary Linux configuration-two compiler was also run on explicit
four-ALU programs which compute common RGB and separately select texture 0
or texture 1 alpha.  They are not required for correctly translated XRGB and
BGRA views, but they prove the physical source swizzles and provide closed
fallback objects if a future view path cannot express alpha one in A30.

Texture-0-alpha times primary alpha:

```text
0140b6d0 3003f000 00f00005 000003e0
0140b6d0 50040055 00703f00 000003e0
0140b6d0 b0040b6b 008200b6 200003e0
01c00530 001c0053 00f03f00 000d0820
```

Its physical ALU SHA-256 is
`d8af75a0c403ff40be0c98d40584f8af9ba5652ed6ead1afa7ae448abce268c3`.

Texture-1-alpha times primary alpha:

```text
0140b6d0 3003f000 00f20005 000003e0
0140b6d0 50040055 00703f00 000003e0
0140b6d0 80040b6b 00803fb6 200003e0
01c00530 001c0053 00f03f00 000d0820
```

Its physical ALU SHA-256 is
`d40b5bc84c204161e48a3644568b2587cff64b7d95108be6a09c29d9db5c82f0`.

The dynamically executed Windows D3D compiler independently emitted four
ALUs and two TAUs for both programs.  Their finalized virtual-ALU hashes are
`1c179d215b9c7c98728473310a1ad3a6c99a53cd0e7530a5d66ac6612199edf6`
and `8b35fc9e8058ee990bbf12f5ca5c78a3a58ddc8abaabaa9f7cce01bbe7a23fa6`;
both use the ordinary virtual TAUs `201001b0 211111b1`.  Do not upload these
virtual words or splice their alpha ALU into another object.  Prefer the
shorter full-vector object plus exact format translation.

## Bounded Mesa classifier

Normalize only identity MOVs and associative, unsaturated four-lane FMULs.
The RGB expression must flatten to the multiset `{tex0.rgb, tex1.rgb,
COL0.rgb}`.  Each sample must be non-array, non-shadow 2D, use its matching
texture/sampler unit, and source XY plus projector W from the matching TEX
varying.  Reject constants, discard, derivatives, alternate outputs,
additional samples, partial color stores, coordinate arithmetic, and any
nonidentity source modifier.

Normalize alpha independently.  Admit only the product of `COL0.a` and the
alpha leaves whose translated sample formats are 99.  A format-98 leaf is
implicit one and must be absent from the normalized NIR alpha expression.
This recognizes both live nested-`vec4` shapes without equating arbitrary
RGB-only shaders to a full-vector multiply.

At draw time require:

* exactly two bound sampler states and views, both guarded and level zero;
* the view A30/alpha semantics above;
* finite ordinary S/T for each post-TNL vertex and both Q values bitwise one;
* normalized D1 via FVF A02 `020041d1` and perspective-correct common A01;
* capability 25 plus the existing base ioctl capability; and
* every other existing pitch, size, filter, wrap, scissor, BO-distinctness,
  guard, fence and retirement condition.

For ioquake's optimized world path, all three post-TNL D1 values are
`ffffffff`.  That white-primary specialization may continue using the live
two-factor object and does not need capability 25.  The classifier above is
for the nonwhite general path and must not depend on observing white in only
one vertex.

## Exact command-13 state-complete map

The current two-texture+D16+blend builder has a 270-dword two-factor body.
The full three-factor object adds one four-dword ALU, making 274 dwords.
Alpha test occupies an existing destination-state slot and adds no length.
For this closed packet:

| state | body index/value |
|---|---|
| destination A52 | `6`, format 98 or 99 plus pitch |
| alpha-test A33 | `19`, or `33007000` when disabled |
| texture 0 A30 | `29`, format 98 or 99 |
| texture 1 A30 | `49`, format 98 or 99 |
| common A00 | `67`, `00100001` for alpha-test-only |
| common A01 | `68`, `01ce154c` write or `01ce1548` read-only |
| depth A10/A11/A12/A13/A20 | `71..75` |
| PS A92 | `198`, `92020003` |
| TAUs | `216..219` |
| texture product | `224..227` |
| primary product | `228..231` |
| output MOV | `232..235` |
| program terminators | `236..237` |
| immediate H2 header | `240..243` |
| complete body | 274 dwords |

Depth LEQUAL is A13 `13030000`; EQUAL is `13020000` behind its separate
capability.  The four-ALU diagnostic objects would move the output to
`236..239`, terminators to `240..241`, H2 to `244..247`, and make the body
278 dwords.  They must use their own complete constant arrays and assertions,
not an in-place edit of the three-ALU packet.

## Later alpha-tested surfaces

No live dump has yet reached an alpha-tested q3dm1 surface, so the shader
shapes here are source- and state-tracker-exact targets, not claimed live
captures.  Gallium advertises fixed alpha test, so the linked fragment NIR
must remain one of the ordinary projected MODULATE forms; there must be no
NIR discard.  The kernel applies A33 after fragment color generation and a
failed test suppresses both destination and depth writes.

ioquake's three directives are exact:

| directive | A33 | common use |
|---|---:|---|
| `GT0` | `33004000` | binary/cutout alpha greater than zero |
| `LT128` | `33001202` | inverted half-alpha mask |
| `GE128` | `33006202` | ordinary grate/foliage half-alpha mask |

The first bounded masked-surface tuple should be one projected BGRA texture
times primary RGBA, blend disabled, full color mask, D16 LEQUAL with writes,
and GEQUAL/ref 128.  It uses command 12 (`0x12`) because only that
state-complete one-texture D16 packet has the noncolliding A33 slot:

```text
body[19]      A33 = 33006202
body[46]      A53 = 53010c0f
body[47]      A00 = 00100001
body[48]      A01 = 01ce154c
body[51..55]  A10,A11,A12,A13,A20
body[208]     immediate H2 header for the MODULATE form
body length   236 dwords
```

The texture must be format 99 if its stored alpha drives the test.  For an
RGB/XRGB texture, format 98 makes fragment alpha come from primary color;
that is legal only when the normalized NIR alpha expression says so.  Never
force BGRA merely because alpha test is enabled.

The common two-pass grate follow-up is a distinct tuple: alpha test disabled,
`DST_COLOR,ZERO`, EQUAL, and read-only D16.  It must use A00 `00000011`, A13
`13020000`, and A01 `01ce1548`; it must not inherit A33 from the first pass.
For q3dm1 this is not merely a global shader-count pattern: it is the exact
second `protobanner` stage.  Its remaining blend words are:

```text
A33 = 33007000
A34 = 34000010
A35 = 35000114
A36 = 36000420
A37 = 37000224
A53 = 53010c0f
```

The stage's `rgbGen identity` must lower to sampled lightmap times exact white
(or an equivalent REPLACE only after proving the alpha result is
unobservable).  Do not retain the banner's interpolated D1 or GE128 state.
Fog and dynamic-light successors are documented separately in
[`ioquake-fog-dlight-successors.md`](ioquake-fog-dlight-successors.md).

## Guarded staged oracles

1. **Four alpha masks.**  Use texture RGB values `(0.8,0.5,0.25)` and
   `(0.5,0.25,0.8)`, primary RGB `(0.5,0.8,0.25)`, and primary alpha `0.5`.
   Store poison alphas `0.25` and `0.6`.  Run translated format pairs
   98/98, 99/98, 98/99, 99/99.  Expected RGB is approximately
   `(0.2,0.1,0.05)` throughout; expected alpha is `0.5`, `0.125`, `0.3`, and
   `0.075`.  Observe alpha in a BGRA target.  Repeat 98/98 after all four
   transitions to catch stale A30 or PS state.
2. **Identity projector gate.**  The same draw with Q0=Q1=one submits.  Set
   Q0 or Q1 alone to `2.0`; each must increment rejection without an ioctl.
3. **Masked color/depth kill.**  Prefill XRGB blue and D16 `ffff`.  A BGRA
   green fragment with alpha `40` under GE128 must preserve both surfaces; a
   second fragment with alpha `80` at the same location must write green and
   its depth.  Verify target, source and D16 guards after retirement.
4. **State closure.**  Run alpha-on GE128, alpha-off COPY, alpha-on GE128
   without context reset.  Then run two-texture primary, legacy two-factor,
   and two-texture primary.  First and last results in each sequence must be
   identical.
5. **Two-pass grate.**  After a passing masked LEQUAL/write draw, issue an
   equal-depth `DST_COLOR,ZERO` read-only draw.  Verify filtered color and
   byte-identical D16, then issue a farther EQUAL draw and require no change.

Every oracle uses distinct guarded BOs, exact submitted/rejected deltas,
cookie retirement, CR/work-idle checks, and an old-program transition.  None
of these results permits nonunit Q, mipmapped views, arbitrary NIR, raw
native programs, or generic texture-format reinterpretation.
