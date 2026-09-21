# ioquake GL1 texture-format and sampler promotion plan

This note narrows the next Chrome9 sampler work to the formats most likely to
appear in ioquake-era GL1 uploads: ARGB8888, RGB565, A8, L8, L8A8,
ARGB1555, and ARGB4444.  It separates exact static decoder evidence from
state already measured on VX900 and gives one standardized guarded test shape
for level-zero and two-level sampling.

No new format should be advertised merely because its A30 byte is known.
Every row still needs a live decoder readback; stored-alpha rows additionally
need an alpha-preserving target or blend discriminator.

## Independent evidence and confidence

The generation-specific Windows binaries are:

```text
VTGUModeDX32A.dll
  sha256 8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll
  sha256 a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
```

D3D's public-to-private converter is `VTGUModeDX32A:0x100c7590`.  Its sampler
lookup at `0x1020a660` maps private A8/L8/A8L8/RGB565/ARGB1555/ARGB4444 to
format bytes `1b/13/15/89/8a/8b`.  The OpenGL ICD independently carries the
same lookup at `VTGOGL32A:0x1025d590`, with the relevant return blocks at
`0x1025d5de`, `0x1025d5e5`, `0x1025d64c`, `0x1025d661`, `0x1025d65a`, and
`0x1025d653`.

The VX900 manual's type-02 image A30 table on printed pp. 124--126 gives the
same selectors and missing-channel behavior.  Its sampler A30--A32 fields on
printed pp. 128--130 give the range/bias, filter, and address bit positions
used below.

The proprietary Linux Xorg H6 table at `via_drv.so:0x86640` independently
maps XRender RGB565/ARGB1555/ARGB4444/ARGB8888/A8 to
`89/8a/8b/99/1b`.  It has no L8 XRender picture row, which is an API-table
absence rather than contradictory hardware evidence.  The exact Xorg binary
SHA-256 is
`5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787`.

Old open Mesa selects the same families in
`Mesa-7.11.2/src/mesa/drivers/dri/unichrome/via_tex.c:445..475`.  Its exact
register definitions in `via_3d_reg.h:969..1027` reduce to L8=`13`,
AL88=`15`, A8=`1b`, RGB565=`89`, ARGB1555=`8a`, ARGB4444=`8b`,
XRGB8888=`98`, and ARGB8888=`99`.  This is an older generation and is
semantic corroboration; agreement between both Windows Chrome9 UMDs and the
VX900 manual is the generation-specific proof.

| API/Gallium format | image A30 | Windows D3D | Windows OGL | Linux Xorg | old Mesa | VX900 status |
|---|---:|:---:|:---:|:---:|:---:|---|
| `B8G8R8X8_UNORM` / XRGB8888 | `30980000` | yes | yes | yes | yes | **live** |
| `B8G8R8A8_UNORM` / ARGB8888 | `30990000` | yes | yes | yes | yes | **live**, sampled alpha observed through source-over |
| `B5G6R5_UNORM` / RGB565 | `30890000` | yes | yes | yes | yes | high-confidence static |
| `B5G5R5A1_UNORM` / ARGB1555 | `308a0000` | yes | yes | yes | yes | high-confidence static |
| `B4G4R4A4_UNORM` / ARGB4444 | `308b0000` | yes | yes | yes | yes | high-confidence static |
| `L8_UNORM` | `30130000` | yes | yes | no picture row | yes | high-confidence static |
| `A8_UNORM` | `301b0000` | yes | yes | yes | yes | high-confidence static |
| `L8A8_UNORM` / A8L8 | `30150000` | yes | yes | no picture row | yes | high-confidence static |

“Live” here applies to the format decoder plus the already-bounded
level-zero path.  The 16-/8-bit rows have not run on this VX900.  No row is
evidence for render-target, scanout, tiling, or external dma-buf support.

## Exact memory and channel semantics

Hardware format names describe the numeric little-endian word; Gallium names
describe components in increasing byte/bit address order:

| logical format | cpp | packed source | decoded RGBA |
|---|---:|---|---|
| XRGB8888 | 4 | dword `00RRGGBB` | `R,G,B,1` |
| ARGB8888 | 4 | dword `AARRGGBB` | `R,G,B,A` |
| RGB565 | 2 | word `RRRRRGGG GGGBBBBB` | `R,G,B,1` |
| ARGB1555 | 2 | word `ARRRRRGG GGGBBBBB` | `R,G,B,A` |
| ARGB4444 | 2 | word `AAAARRRR GGGGBBBB` | `R,G,B,A` |
| L8 | 1 | byte `L` | `L,L,L,1` |
| A8 | 1 | byte `A` | `0,0,0,A` |
| L8A8 | 2 | low byte L, high byte A | `L,L,L,A` |

The decoder provides these legacy missing-channel values.  A30 has no general
RGBA swizzle bits: bit 15 selects high-bit versus zero extension, bit 14 is
packed 1/2/4-bit texel ordering, and bits `[13:11]` route sampled depth.
Arbitrary sampler-view swizzles still require fragment-program lowering.

This distinction matters for GL1 alpha textures.  Native A8 exactly matches
Gallium's `PIPE_FORMAT_A8_UNORM` value `000A`; a legacy GL base-format view
which requires different RGB constants must show that swizzle explicitly in
NIR and use a matching shader key.  Enabling A8 storage must not silently
classify such a program as ordinary four-lane REPLACE or MODULATE.

## Sampler state is format-independent

Image A30 above belongs to the type-02 image packet.  Sampler A30 is a
different register namespace and always carries LOD range/bias:

```text
sampler A30 = 30000000
            | ((bias_s5_5 & 03ff) << 12)
            | (maximum_level << 6) | minimum_level
```

The paired Windows builders at `0x101d9380/0x10241ba0` establish independent
S/T minification and magnification fields plus the D/mip filter.  For ordinary
2D sampling:

```text
A31 = 31100000
    | (mag << 13) | (min_image << 10)
    | (mag <<  7) | (min_image <<  4)
    | mip

nearest = 0, linear = 1
mip none = 0, nearest = 1, linear = 2
```

The complete six GL min-filter combinations, shown first with nearest
magnification and then with linear magnification, are:

| GL minification | mag nearest A31 | mag linear A31 |
|---|---:|---:|
| NEAREST | `31100000` | `31102080` |
| LINEAR | `31100410` | `31102490` |
| NEAREST_MIPMAP_NEAREST | `31100001` | `31102081` |
| LINEAR_MIPMAP_NEAREST | `31100411` | `31102491` |
| NEAREST_MIPMAP_LINEAR | `31100002` | `31102082` |
| LINEAR_MIPMAP_LINEAR | `31100412` | `31102492` |

Old Mesa `via_state.c:813..864` independently makes the same semantic split
between min-image, mip, and magnification filters.  Its older registers are
not used as Chrome9 bit-position evidence.

The Windows address builders at `0x101d9250/0x10241a70` map API WRAP,
MIRROR, CLAMP, BORDER, and MIRRORONCE to hardware codes `2,3,1,0,7`.
The bounded current set is:

```text
A32 = 32000040 | (wrap_t << 3) | wrap_s

clamp/clamp   = 32000049
repeat/repeat = 32000052
mirror/mirror = 3200005b
```

On VX900, a guarded 30x18 BGRX image has already passed clamp, repeat, and
mirrored repeat under both nearest and linear sampling, including the
expected four-color `0x7f/0x80` linear average.  That is measured evidence for
the level-zero state on selector 98.  Independent min/mag combinations and
all mip-filter codes remain static until their respective oracles pass.
Border and mirror-once remain outside the current UAPI.

## Canonical pitch and a common two-level test object

For every local linear level:

```text
pitch[l] = ALIGN(width[l] * cpp, 32)
size[l]  = pitch[l] * height[l]
```

There is no independent sampler pitch register.  Canonical 32-byte row
padding does not change A20's logical width; a noncanonical imported pitch
changes effective width in the proprietary builder and therefore continues
to require the existing guarded shadow.

Conveniently, all requested formats fit the same current guarded 32x32 plus
16x16 object geometry on a 4 KiB-page host:

| cpp | L0 pitch/size | L1 pitch/size | L0 object end | L1 object offset | complete size |
|---:|---:|---:|---:|---:|---:|
| 1 | `20/0400` | `20/0200` | `3000` | `3000` | `5000` |
| 2 | `40/0800` | `20/0200` | `3000` | `3000` | `5000` |
| 4 | `80/1000` | `40/0400` | `3000` | `3000` | `5000` |

L0 begins at object offset `0x1000`; the retained internal guard is
`0x2000..0x2fff`; L1 begins at `0x3000`; and the final guard is
`0x4000..0x4fff`.  Therefore every row above uses the same SF-relative table
entry and complete H3 packet:

```text
FE03043C 00000008 10030000 CC000000
40000020 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

A20=`20020020` and A22=`22008055` also remain constant.  Only cpp-derived row
layout, source bytes, and image A30 change.  This is a particularly safe
cross-format mip discriminator because the PTE count, level addresses,
dimensions, packet length, and guards are identical.

## Safest level-zero decoder vectors

Use the live one-texture REPLACE object, a BGRA8888 destination A52=`99`,
nearest A31=`31100000`, clamp A32=`32000049`, and four constant 16x16
quadrants.  Sample quadrant centers so neither filtering nor edge behavior
can hide a decoder error.

| image A30 | quadrant source values | expected BGRA dwords |
|---:|---|---|
| ARGB8888 `30990000` | `00ff0000,80ff0000,ff00ff00,ff0000ff` | identical dwords |
| RGB565 `30890000` | `f800,07e0,001f,ffff` | `ffff0000,ff00ff00,ff0000ff,ffffffff` |
| ARGB1555 `308a0000` | `7c00,fc00,83e0,801f` | `00ff0000,ffff0000,ff00ff00,ff0000ff` |
| ARGB4444 `308b0000` | `0f00,ff00,f0f0,f00f` | `00ff0000,ffff0000,ff00ff00,ff0000ff` |
| L8 `30130000` | `00,55,aa,ff` | `ff000000,ff555555,ffaaaaaa,ffffffff` |
| A8 `301b0000` | `00,55,aa,ff` | `00000000,55000000,aa000000,ff000000` |
| L8A8 `30150000` | `0000,ff00,0080,ffff` | `00000000,ff000000,00808080,ffffffff` |

Poison every row tail and allocation gap with a value which decodes to none
of the expected colors.  Validate source bytes, padding, destination, both
object guards, retirement cookie, CR PAUSE, and engine work status after each
draw.  A timeout quarantines both BOs.

After a row passes nearest, change only A31 and test its decoded linear
components at a two-texel midpoint:

- RGB565 red/green must yield R/G independently in `{0x7f,0x80}`, B=0,
  A=`ff`;
- L8 `00/ff` must yield equal RGB in `{0x7f,0x80}`, A=`ff`;
- A8 `00/ff` must retain RGB zero and yield A in `{0x7f,0x80}`;
- L8A8 `(L=0,A=ff)/(L=ff,A=0)` must yield all four channels in
  `{0x7f,0x80}`; and
- stored-alpha RGB16 endpoints should first use alpha 0/1; fractional alpha
  filtering is a later rounding test.

## Per-format two-level discriminator

Do not combine a new decoder with the first mip-address test.  First pass the
two-level ioctl using live XRGB.  Then reuse the common `0x5000` object above,
nearest spatial filtering, and these unmistakable L0/L1 fills:

| format | L0 fill | L1 fill | forced-L0 / forced-L1 expected |
|---|---:|---:|---|
| ARGB8888 | `20ff0000` | `e000ff00` | translucent red / high-alpha green |
| RGB565 | `f800` | `07e0` | opaque red / opaque green |
| ARGB1555 | `7c00` | `83e0` | transparent red / opaque green |
| ARGB4444 | `0f00` | `f0f0` | transparent red / opaque green |
| L8 | `20` | `e0` | dark gray / light gray, A=1 |
| A8 | `20` | `e0` | RGB zero, low / high alpha |
| L8A8 | `e020` | `20e0` | dark/high-alpha / light/low-alpha gray |

For each format run unbiased L0, bias `+32` s5.5 forced L1, and unbiased L0
again:

```text
L0: A30/A31 = 30000040 / 31100000
L1: A30/A31 = 30020040 / 31100000
L0: A30/A31 = 30000040 / 31100000
```

Here A30 is the sampler LOD word, not the format word.  Require the exact
L0/L1/L0 colors and alpha plus all padding/guards.  Only after address
selection passes should add nearest-mip and trilinear A31 codes for that cpp.

## Kernel/Mesa containment boundary

The first packed-format UAPI must carry a normalized enum.  The kernel maps
that enum to `{image_A30, cpp}`, derives every pitch/size/level offset, and
rejects any object whose exact guarded size does not match.  Never let
userspace pair a one-byte cpp with selector 89 or submit a raw A30 word.

Keep the old level-zero and mip ioctl validators limited to their advertised
format set.  Add a format only after its raw gate, preferably with a distinct
capability bit so Mesa never infers packed support from the generic textured
cap.  PRIME import/export and scanout remain BGRX-only until their separate
layout/fourcc paths exist; the first packed resources are local sampler-only
BOs.

Mesa currently hard-codes surface cpp as Z16=2 and color=4 in resource layout
and transfers.  Before native packed allocation, centralize a checked
format-to-cpp helper and use it for layout, transfer X offsets, shadow copies,
and UAPI pitch validation.  `is_format_supported` should initially return
only `PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR` for the new rows.  Sampler
views must validate the native default swizzle or select an exact shader key;
they must not assume all formats have identity RGBA.

The safest promotion order is:

1. retain the already-live ARGB8888 path;
2. RGB565 sampler-only, because its alpha is fixed one and its primary-color
   vector is unambiguous;
3. L8 sampler-only, important for legacy lightmaps and natively `LLL1`;
4. A8, only with the required NIR/view-swizzle shader shapes;
5. L8A8, ARGB1555, and ARGB4444 after BGRA alpha readback; and
6. mip filtering per format only after level-zero nearest/linear and the XRGB
   two-level table pass.

Until a native row passes, expand uploads into the live 32-bit backing:

```text
RGB565   -> R8,G8,B8,ff
L8       -> L,L,L,ff
A8       -> 00,00,00,A
L8A8     -> L,L,L,A
ARGB1555 -> R8,G8,B8,(A1 ? ff : 00)
ARGB4444 -> replicate each four-bit component
```

This fallback preserves sampling semantics and mip generation, but not exact
packed storage, packed render targets, or external-image layout.  Reject
those exact-layout binds until their independent paths are proved.
