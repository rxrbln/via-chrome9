# Chrome9 texture-coordinate and LOD contract

This note separates three controls which are easy to conflate during the
glmark2 texture bring-up: the coordinate representation named by the vertex
output declaration, projective division by a texture Q, and mip selection
from screen-space derivatives.  It records only fields recovered from the
VX900 manual and executable driver paths.  Unmeasured native modes remain
diagnostic gates rather than Gallium capability claims.

## Evidence and confidence

The exact register fields below come from the VX900 3D manual's type-02/FE
register pages 136 and 138.  The projection transition is independently
present in the supplied Windows D3D and OpenGL UMDs and in the proprietary
Linux GL driver:

| evidence | entry or range | result |
|---|---:|---|
| Windows D3D | `VTGUModeDX32A.dll:0x101d9c00..0x101d9cfc` | clear/set image A18 bit 11 and dirty the stage |
| Windows OpenGL | `VTGOGL32A.dll:0x10242420..0x1024251c` | byte-identical projection builder |
| D3D vertex layout | `0x101f6643..0x101f6a85` | distinguish 1--4 components; enable projection for four |
| D3D closure | `0x101f7087..0x101f709b` | explicitly disable projection on fallback |
| proprietary Linux | `STM_TexTransformFlag_H5:0x1b1900` | clear/set the same image-state bit |
| proprietary Linux | `__SetupTexProjection_invi:0x1b1f70` | independent builder for the same transition |

The 256-byte Windows projection-builder regions have identical SHA-256
`757fb7aa210d2436a1eff31718d40b55bdd3a0d233e2a3a81ee18f425a7e0c95`.
The D3D wrapper at `0x101d9d10..0x101d9d42` reads internal transform flag
bit 8 before calling the builder; this agrees with the public
`D3DTTFF_PROJECTED=0x100` definition.  The OpenGL copy is at
`0x10242530..0x10242562`.

This establishes image A18 bit 11 and its lifecycle with high confidence.
The complete native-projective A01/A08 tuple is field-derived from the
manual but has not yet been captured or run on silicon.  The A08 normalized
input modes and A04 screen replacement are exact manual fields, but their
numeric effects have not been measured on this chip.  They must not be used
to advertise rectangle textures until the bounded discriminators below pass.

## A08 describes vertex-output coordinates

Type-02/FE A08 is `Texture A-H Control 1`.  For texture A the exact payload
fields are:

```text
A08[13]    S coordinate input mode: 0 un-normalized, 1 normalized
A08[12]    T coordinate input mode: 0 un-normalized, 1 normalized
A08[11]    R coordinate input mode: 0 un-normalized, 1 normalized
A08[10]    Q coordinate input mode: 0 un-normalized, 1 normalized
A08[9:8]   dimension: 00 S, 01 ST, 10 STR, 11 STRQ
A08[3:0]   number of texture-coordinate output registers
```

Texture B repeats the coordinate-mode bits at 21:18 and dimension at 17:16.
The live one-texture word is:

```text
A08 = 08010101
```

It declares one texture-coordinate register and 2-D S/T for texture A, with
A's S/T mode bits clear.  Xorg's executable path divides S by the effective
texture width and T by height before emitting this state
(`via3DEmitQuad_H5:0x2e6a8..0x2e727`).  Thus clear bits are already known to
work with ordinary `[0,1]` coordinates, despite the manual's potentially
counterintuitive label `Un-normalized`.  Do not conclude that setting the
bits is required for normalized GL sampling.

The exact one-bit diagnostic words are:

```text
baseline             08010101
set A S mode          08012101
set A T mode          08011101
set A S and T modes   08013101
```

Use an asymmetric 8x4 texture with unique texels.  Draw the same interior
samples with `[0,1]` S/T and change only A08.  Then repeat with texel-space
coordinates.  Sample away from edges so a half-texel convention cannot hide
the result.  A timeout quarantines all mappings.  A visible difference is
only a mode characterization; it does not prove correct wrap or derivative
semantics.

## Rectangle sampling: use a safe lowering first

The supplied proprietary OpenGL driver advertises
`GL_EXT/ARB/NV_texture_rectangle`.  `VTGOGL32A.dll` contains 28 immediate
uses of `GL_TEXTURE_RECTANGLE=0x84f5` in its API front end, including target
validation at `0x1014402d`.  No rectangle-specific branch was found in the
paired Chrome9 image, filter, address, or LOD builders.  Therefore API
acceptance is evidence for a shipping implementation, but not evidence that
A08's undocumented-on-silicon setting directly implements rectangle
coordinates.

The safe initial Gallium lowering is explicit:

```text
s_hw = s_rect / level_width
t_hw = t_rect / level_height
A08  = 08010101
```

Keep the ordinary image A20/A22, A01 perspective correction, A18 location,
and sampler words.  For an 8x4 image, texel centers `(0.5,0.5)` and
`(7.5,3.5)` become `(0.0625,0.125)` and `(0.9375,0.875)`.  Compare a direct
normalized draw with `normalized -> lowered rectangle -> normalized` in one
context.  The first and third images must be byte-identical and guards must
remain unchanged.

This lowering also preserves LOD derivatives: dividing rectangle S/T by the
level dimensions before the hardware's ordinary normalized-image path makes
the derivative in texels equal to the original rectangle-coordinate
derivative.  It is preferable to changing A08 until the latter's filtering
and wrap behavior is proven.

## A04 is screen-coordinate replacement, not rectangle mode

Type-02/FE A04 is a separate POT-oriented coordinate generator:

```text
A04[23:20] = width exponent Wexp; replacement s = screen_x / 2^Wexp
A04[19:16] = height exponent Hexp; replacement t = screen_y / 2^Hexp
A04[7:0]   = one replacement-enable bit per texture stage
```

For stage 0 the exact examples are:

```text
32x32 replacement enable   A04 = 04550001
8x4 replacement enable     A04 = 04320001
replacement disabled       A04 = 04000000
```

This is useful for a screen-aligned POT sprite, but it cannot represent an
arbitrary NPOT divisor.  A bounded diagnostic uses an 8x4 nearest/clamp
texture, supplies poison S/T, enables `04320001`, and checks interior pixels
against `screen_x/8, screen_y/4`.  Immediately write `04000000` and repeat
with explicit S/T.  Never omit A04 from a state image after testing the
replacement path; the enable is sticky state.

## Perspective correction and projection are distinct

Ordinary S/T uses the already-live general texture word:

```text
A01 = 01002521
stage-0 perspective mode = 01       perspective correction
source                   = 000      texture coordinate A
dimension                = 01       S/T
```

This combines S/T with the position reciprocal W.  Projective texturing by
an independent Q is different.  Both Windows UMDs locate the stage image
record as `(image_state + 0x290 + stage*0xc4)` and update dword `+0x60` as
follows:

```text
clear:  word &= fffff7ff
set:    word ^= ((enable << 11) ^ word) & 00000800
```

The set is suppressed for two internal formats and one resource class.  The
old and new bit are compared, and only a change dirties the corresponding
hardware-stage record.  The exact translated-SF image transition is:

```text
ordinary image       A18 = 18001101
projected class      A18 = 18001901
```

The D3D FVF/layout path stores component counts 3, 4, 1, and 2 at
`0x101f66e0`, `0x101f67d1`, `0x101f6905`, and `0x101f6984`; at
`0x101f6a3b..0x101f6a64` it enables the projection builder exactly when the
selected count is four.  This closes the four-component input requirement.

The manual-derived full native candidate is:

```text
A18 = 18001901                  image projection class
A01 = 01002543                  projection mode 10, source A, STRQ
A08 = 08010301                  texture A dimension STRQ
```

Only A18 bit 11 has executable cross-driver proof.  Do not submit the full
tuple outside a guarded diagnostic until an emitted vendor packet or a live
silicon result confirms it.  The first implementation remains the established
software transform:

```text
rhw' = rhw * q
s'   = s / q
t'   = t / q
```

with ordinary A01/A08/A18.  It is valid for one independently projected
texture with finite nonzero Q.  It cannot carry two different texture-Q
denominators in the single position reciprocal-W field.

## LOD words tied to coordinate tests

The D3D and OpenGL LOD builders at `0x101d9a40` and `0x10242260` are
byte-identical and match Linux `STM_MipMapLOD_INV:0x1b1a50`:

```text
A30 = 30000000
    | ((signed_bias_s5_5 & 03ff) << 12)
    | ((maximum_level & 3f) << 6)
    |  (minimum_level & 3f)
```

Exact bias and L0--L1 words are:

```text
bias  0.0, levels 0..1   30000040
bias +0.5, levels 0..1   30010040
bias +1.0, levels 0..1   30020040
bias -0.5, levels 0..1   303f0040
bias -1.0, levels 0..1   303e0040
```

For nearest spatial filtering plus nearest mip selection, use
A31=`31100001`; for trilinear use `31102492`.  Keep A32 explicitly written.
The existing address-table contract still applies: L0 is in the image block,
L1--L8 occupy the complete eight-entry type-03/subtype-10 table, and every
reachable level must be mapped and retained.

The most useful coordinate/LOD discriminator uses red 64x32 L0 and green
32x16 L1:

1. Draw to 64x32 with normalized coordinates spanning 0..1.  With unbiased
   nearest-mip state, the interior must select L0.
2. Draw to 32x16 with the same span.  The two-source-texels-per-pixel
   derivative must select L1.
3. Repeat step 2 with rectangle inputs spanning 0..64/0..32 and software
   division by 64/32.  It must match the normalized draw byte for byte.
4. On the 1:1 draw, bias `+1` must select L1 and restoring `0` must return
   to L0.  On the 2:1 draw, bias `-1` must select L0 and restoring `0` must
   return to L1.  This checks both bias signs without relying on a level
   outside the two-level allocation.

The bias sequence is a field-polarity test, not permission to address a level
outside the table.  Clamp min/max before submission; never program a maximum
whose explicit entry or proven implicit tail is absent.

## Transition closure and capability policy

Every target or coordinate transition must rewrite the full affected state:

```text
A04                         screen replacement, normally 04000000
A08                         coordinate modes, dimensions, output count
general A01                 perspective/projection source and dimension
image A18/A20/A22           projection class, exact dimensions and POT flag
sampler A30/A31/A32         level bounds, bias, filters and wrap
complete reachable table    initialized L1--L8 entries
```

For the present driver, truthful normalized `PIPE_TEXTURE_2D` sampling keeps
A08=`08010101`.  `PIPE_TEXTURE_RECT` can be supported by the explicit
software/NIR divide while retaining the same hardware state.  Native A08
normalization, A04 replacement, and the A18/A01 projective tuple remain
diagnostic features until their enable/disable/enable oracles pass on real
hardware without a timeout, guard change, or stale-state failure.
