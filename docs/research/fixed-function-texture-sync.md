# Fixed-function, texture-shape, and presentation state

## Scope and evidence

This note closes a set of state gaps around the packet-exact draw oracles.  It
uses the VX900 Chrome9 HD manual, the symbol-rich Linux OpenGL driver
`via_chrome9_dri.so`, and both Windows user-mode drivers.  Addresses prefixed
`VTGUModeDX32A` are virtual addresses in the Direct3D UMD; addresses prefixed
`VTGOGL32A` are virtual addresses in the OpenGL ICD.  Unprefixed addresses are
ELF symbol/RVA addresses in `via_chrome9_dri.so`.

The strongest cross-check is that the Windows D3D and OpenGL builders are
byte-identical for color masks, alpha reference/function, texture address and
filter modes, fog state, logic operations, blending, and stencil.  The Linux
ICD uses separately compiled implementations with the same state layout and
the same lookup tables.  These are live shared hardware-state routines, not
names guessed from the manual.

## Texture formats and component order

Type-02 subaddress `30[23:16]` is the complete texture-format selector.  It
does not contain a general component-swizzle field.  The exact Windows lookup
is `VTGUModeDX32A:0x1020a660`, duplicated at
`VTGOGL32A:0x1025d590`; the public-to-private D3D converter is at
`VTGUModeDX32A:0x100c7590`.  The complete accepted table is in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  The common color
formats are worth spelling out because the names are word-order names, while
Mesa formats describe byte order:

| A30 byte | hardware name | little-endian memory word | Mesa/Gallium interpretation |
|---:|---|---|---|
| `88` | X1R5G5B5 | `0RRRRRGG GGGBBBBB` | B5G5R5X1 |
| `89` | R5G6B5 | `RRRRRGGG GGGBBBBB` | B5G6R5 |
| `8a` | A1R5G5B5 | `ARRRRRGG GGGBBBBB` | B5G5R5A1 |
| `8b` | A4R4G4B4 | `AAAARRRR GGGGBBBB` | B4G4R4A4 |
| `98` | X8R8G8B8 | `00RRGGBB` | B8G8R8X8 |
| `99` | A8R8G8B8 | `AARRGGBB` | B8G8R8A8 |
| `9a` | A2R10G10B10 | `AARRRRRR RRRRGGGG GGGGGGBB BBBBBBBB` | B10G10R10A2 |
| `b8` | X8B8G8R8 | `00BBGGRR` | R8G8B8X8 |
| `b9` | A8B8G8R8 | `AABBGGRR` | R8G8B8A8 |
| `ba` | A2B10G10R10 | `AABBBBBB BBBBGGGG GGGGGGRR RRRRRRRR` | R10G10B10A2 |
| `bb` | G16R16 | `GGGGRRRR` in 16-bit components | R16G16 with B=A=1 |

These layouts are stated by the manual's A30 table and selected by both
Windows UMDs.  The manual has typographical errors in its later RGBA/BGRA
descriptions, so the lookup and the explicit bit diagrams above take
precedence over repeated prose labels.

Four entries which had previously been left as anonymous private codes can
now be bounded more tightly from the D3D format converter at
`VTGUModeDX32A:0x100c7590`:

| public/private source | private format | A30 byte | proved interpretation |
|---|---:|---:|---|
| FourCC `YV12` (`32315659`) | `080a` | `0b` | planar YV12; `0b` is the per-plane 8-bit intensity fetch, not a complete packed YV12 texel |
| FourCC `AI44` / `A4I4` | `080e` | `33` | AI44, A in the high nibble and palette index in the low nibble |
| FourCC `AYUV` (`56555941`) | `2044` | `31` | packed A/Y/U/V in bits `31:24/23:16/15:8/7:0` |
| private `182d` | `182d` | `b8` | native X8B8G8R8 fetch, but no proved public D3D origin |

The YV12 classification is independently reinforced by the resource paths.
The constructor at `VTGUModeDX32A:0x100d0dd9..0x100d0df0` marks YV12 with
resource flag `00100000` before storing private code `080a`, and texture-copy
paths at `0x101d0f81..0x101d0fb5`, `0x101cfbf0..0x101cfc3a`, and
`0x101d2174..0x101d21c2` special-case `080a` together with planar relatives.
They derive the chroma extent from half dimensions and grow the covered range
to roughly one-and-a-half luma planes.  The manual likewise assigns A18
location fields `[1:0]`, `[3:2]`, and `[5:4]` to Y, Cb, and Cr buffers.  Thus
advertising `080a` as ordinary I8 would fetch only the luma plane and silently
omit chroma conversion and plane addressing.

The normal sampler builder at `VTGUModeDX32A:0x101d7c10` makes the separation
even sharper.  After calling the format lookup, only private `080e` (AI44),
`1023` (YUY2), and `2044` (AYUV) enter its packed-video branch at
`0x101d7d36..0x101d7d6f`; `080a` YV12 does not.  For the normal RGB render
target case that branch sets A30 `[1:0]=01`, the manual's fixed BT.601
conversion, at `0x101d802a..0x101d804a`.  Thus the normal packed-video words
are `30330001` for AI44, `30300001` for YUY2, and `30310001` for AYUV before
any unrelated A30 controls.  A special YUY2-target case leaves `[1:0]=00`,
which treats Y/U/V as G/R/B and expects conversion elsewhere; it should not be
generalized to ordinary color targets.

AI44 also has mandatory palette state.  When the lookup returns `33`, the
same builder copies exactly 16 consecutive four-byte entries into the
hardware palette image (`0x101d7db6..0x101d7f46`) and marks the palette block
dirty.  This agrees with the manual's type-03 subtype-00 table: each 24-bit
entry is Y8/U8/V8 and the texel's low nibble indexes it while its high nibble
supplies alpha.  Consequently AI44 is not usable from A30=`33` alone.

There is one useful negative result.  Public `D3DFMT_R8G8B8` (enum 20) maps
to private `182f` at `0x100c75cf`, whereas the sampler lookup accepts
`182d -> b8` and rejects `182f`.  Either another surface path canonicalizes
the private representation or this is a non-sampleable/stale path; the two
codes must not be conflated.  In the initial Gallium format table, keep YV12,
the palette-index formats, AYUV, and 24-bit RGB disabled until their complete
plane/palette/conversion state or canonicalization has been recovered.

Luminance, alpha, depth, compressed, float, bump, and YUV codes are also
format selectors, not swizzles.  In particular:

* `13` L8 replicates L into RGB and supplies alpha one;
* `14` A4L4 and `15` A8L8 supply L to RGB and the stored alpha to A, while
  `16` L16 supplies a 16-bit luminance and alpha one;
* `1b` A8 supplies RGB zero and stored A;
* `29/2a/2b` are DXT1, DXT2/3, and DXT4/5;
* `f0/f4/f5` are R16F, G16R16F, and R32F; the manual prohibits filtering for
  the floating-color class;
* `fe` samples the 24-bit Z component of D24X8/D24S8 and is documented as
  nearest-only.

Therefore a Gallium sampler view should select one of the two native channel
orders when it can.  Arbitrary `PIPE_SWIZZLE_*` must be lowered in the shader;
writing imagined A30 swizzle bits would instead change the format.  The same
rule explains the live red result: the immediate vertex D3DCOLOR
`ffff0000` is an API ARGB value whose R byte reaches shader R, while an
X8R8G8B8 destination stores little-endian dword `00ff0000`.  A raw memory
byte sequence is consequently B,G,R,X.

A30 bits outside the format byte do not provide a hidden component swizzle.
Bit 15 chooses high-bit versus zero extension for color channels, bit 14 only
reverses the order of 1-, 2-, or 4-bit texels within each byte, and `[13:11]`
chooses which components receive a sampled depth value.  Bit 14 is therefore
applicable to packed palette/intensity indices, not to RGBA channel order.

### Guarded one-delta format oracles

The safest format expansion reuses the measured 32-by-32 nearest/REPLACE draw
unchanged.  Only the source allocation's bytes-per-texel, canonical pitch,
source contents, and texture-format byte change.  A20 remains `20020020`, A22
remains `22008055`, and the sampler remains
`30000000 31100000 32000052`.  In the current kernel-generated body the
format word is exactly `body[25 + blend_delta]`, where `blend_delta` is zero
without the successor blend state and four with it.  None of these direct
REPLACE cases changes the 224/228-dword body lengths or any later packet
index.

Use four 16-by-16 constant quadrants and sample their centers.  Full-scale
channel values deliberately avoid any uncertainty about low-bit expansion or
filter rounding:

| format | A30 | cpp / pitch | TL, TR, BL, BR source words | expected XRGB8888 |
|---|---:|---:|---|---|
| R5G6B5 | `30890000` | 2 / 64 | `f800,07e0,001f,ffff` | red, green, blue, white |
| X1R5G5B5 | `30880000` | 2 / 64 | `7c00,03e0,001f,7fff` | red, green, blue, white |
| A1R5G5B5 | `308a0000` | 2 / 64 | `fc00,83e0,801f,ffff` | red, green, blue, white |
| A4R4G4B4 | `308b0000` | 2 / 64 | `ff00,f0f0,f00f,ffff` | red, green, blue, white |
| X1B5G5R5 | `30a80000` | 2 / 64 | `001f,03e0,7c00,7fff` | red, green, blue, white |
| B5G6R5 | `30a90000` | 2 / 64 | `001f,07e0,f800,ffff` | red, green, blue, white |
| A1B5G5R5 | `30aa0000` | 2 / 64 | `801f,83e0,fc00,ffff` | red, green, blue, white |
| A4B4G4R4 | `30ab0000` | 2 / 64 | `f00f,f0f0,ff00,ffff` | red, green, blue, white |
| X8B8G8R8 | `30b80000` | 4 / 128 | `000000ff,0000ff00,00ff0000,00ffffff` | red, green, blue, white |
| A8B8G8R8 | `30b90000` | 4 / 128 | `ff0000ff,ff00ff00,ffff0000,ffffffff` | red, green, blue, white |
| A2R10G10B10 | `309a0000` | 4 / 128 | `fff00000,c00ffc00,c00003ff,ffffffff` | red, green, blue, white |
| A2B10G10R10 | `30ba0000` | 4 / 128 | `c00003ff,c00ffc00,fff00000,ffffffff` | red, green, blue, white |
| G16R16 | `30bb0000` | 4 / 128 | `00000000,0000ffff,ffff0000,ffffffff` | blue, magenta, cyan, white |

The 16-bit source useful range is exactly `64*32 = 0x800` bytes; the 32-bit
range is `128*32 = 0x1000`.  Keep a complete guard page on both sides, require
a 256-byte-aligned source base, and verify every useful and guard byte after
retirement.  A bounded UAPI should select a kernel-known format enum and let
the kernel derive cpp, pitch, range, and A30; it must never accept a raw A30
byte or a userspace-computed extent.

The `b8/b9` rows are especially useful channel-order discriminators.  Their
red source word is numerically blue in the already-proved `98/99` layout, so
a stale `98/99` selector cannot accidentally pass.  The 16-bit rows exercise
a new cpp and pitch as well as a new decoder.  Enable `89` first: it has no
stored alpha, is present in the XRender texture table, and its full-scale
primary colors have unambiguous results.  The `a8..ab` BGR-order quartet is
also taken from that executable Xorg H6 texture table; unlike the RGB-order
quartet, it is not independently accepted by the Windows public sampler
lookup.  Test each row separately and keep those four gated until silicon
confirms them.

G16R16's blue output for the all-zero source is intentional: the manual says
that native selector supplies B=A=1 while converting the stored low R16 and
high G16 components.  The four endpoint words avoid assumptions about its
intermediate fixed-point precision.  Gallium views which require B=0 must
apply a shader/view swizzle after this native fetch.

Stored alpha needs an independent observation because the bootstrap target is
XRGB.  Once canonical source-over blending is live, keep the pure REPLACE
pixel program, initialize the target to opaque green, and draw two regions
with the same red color.  REPLACE isolates sampled alpha from the packed
vertex-color normalization required by MODULATE:

| format | transparent red | opaque red | expected transparent / opaque |
|---|---:|---:|---|
| A1R5G5B5 | `7c00` | `fc00` | unchanged green / red |
| A4R4G4B4 | `0f00` | `ff00` | unchanged green / red |
| A1B5G5R5 | `001f` | `801f` | unchanged green / red |
| A4B4G4R4 | `000f` | `f00f` | unchanged green / red |
| A8R8G8B8 | `00ff0000` | `ffff0000` | unchanged green / red |
| A8B8G8R8 | `000000ff` | `ff0000ff` | unchanged green / red |
| A2R10G10B10 | `3ff00000` | `fff00000` | unchanged green / red |
| A2B10G10R10 | `000003ff` | `c00003ff` | unchanged green / red |

Use RGB factors `SRC_ALPHA, INV_SRC_ALPHA` and alpha factors
`ONE, INV_SRC_ALPHA`.  Alpha zero and one avoid a rounding-dependent expected
value.  This test must pass before the corresponding Gallium format is called
blend-capable; a successful direct REPLACE test alone proves only its RGB
decode.

The single-channel formats have equally bounded follow-ups.  L8 uses
A30=`30130000`, cpp one, pitch 32, and maps source `00/ff` to black/white with
alpha one.  A8 uses A30=`301b0000`; direct REPLACE is intentionally
uninformative on XRGB, but source-over black A=`00/ff` over white must leave
white / produce black.  The executable Xorg format table additionally accepts
alpha-only A1 and A4 as A30=`30180000` and `301a0000`, respectively.  Both
use a 32-byte row pitch at width 32.  The manual's A30 bit-14 packing rule says
that with the bit clear, A1 texel zero is byte bit 0 and A4 texel zero is the
low nibble.  Prove the otherwise-undocumented A1 selector with source-over
black on a white XRGB target: make each A1 row start with byte `01` and each A4
row start with byte `0f`, with the remaining row zero.  A1 must produce black
at x=0 and white at x=1..7; A4 must produce black at x=0 and white at x=1.
If the black sample appears at x=7 or x=1 respectively, bit-14 ordering was
interpreted backwards.  Both complete pitch-times-height ranges are `0x400`
bytes, so preserve their padding and adjacent guards.  Keep A1/A4 capability
off until this blend-and-order probe passes; the Windows sampler table does
not independently confirm them.

A4L4 uses A30=`30140000`, cpp one, and byte `f0/ff/0f`
means opaque black / opaque white / transparent white.  A8L8 uses
A30=`30150000`; `ff00` is opaque black, `ffff` opaque white, and `00ff`
transparent white for the same blend discriminator.  Do not enable A8, A4L4,
A8L8, or any stored-alpha 16-bit format from the static table alone.
L16 is the missing non-alpha sibling: public `D3DFMT_L16` (enum 81) maps to
private `1028` in `VTGUModeDX32A:0x100c7590`, and the sampler lookup maps
`1028 -> 16`.  With A30=`30160000`, cpp two, pitch 64, source `0000/ffff`
must produce black/white with alpha one.  This extreme-value probe avoids the
manual's unusual intermediate conversion description; defer fractional L16
until its precision and rounding are measured.

The three accepted floating-color selectors also have exact nearest-only raw
decoder probes.  The manual explicitly says this class cannot be filtered,
so retain A31=`31100000`, one level, and no mip derivatives.  Half/float zero
and one are exactly representable and the XRGB target clamps the documented
components without rounding ambiguity:

| format | A30 | cpp / pitch | source words | expected XRGB8888 |
|---|---:|---:|---|---|
| R16F | `30f00000` | 2 / 64 | `0000`, `3c00` | cyan `00ffff`, white `ffffff` |
| G16R16F | `30f40000` | 4 / 128 | `00000000`, `00003c00`, `3c000000`, `3c003c00` | blue, magenta, cyan, white |
| R32F | `30f50000` | 4 / 128 | `00000000`, `3f800000` | cyan `00ffff`, white `ffffff` |

The non-red channels are intentional hardware behavior, not typographical
swizzles: R16F/R32F supply G=B=A=1, while G16R16F supplies B=A=1.  Gallium's
usual single-channel view semantics therefore require shader/view swizzling
after the raw fetch.  For a 32-square source the useful ranges are `0x800`
for R16F and `0x1000` for the 32-bit rows.  Keep negative, infinity, NaN,
linear-filter, and mip cases disabled until separate guarded tests establish
their saturation and unsupported-filter behavior.

Packed YUY2 and AYUV can be separated from the still-unsafe planar/palette
formats with uniform BT.601 extrema.  The Windows builder's normal RGB-target
path supplies the mandatory low conversion bits, so the complete selectors
are A30=`30300001` for YUY2 and `30310001` for AYUV, not merely `30300000`
and `30310000`.  Use nearest REPLACE, one level, and constant 16-by-16
quadrants:

| format | cpp / pitch | black source | white source | expected XRGB8888 |
|---|---:|---:|---:|---|
| YUY2 | 2 / 64 | pair dword `80108010` | pair dword `80eb80eb` | black / white |
| AYUV | 4 / 128 | texel dword `ff108080` | texel dword `ffeb8080` | black / white |

YUY2 bytes are `Y0,U,Y1,V`; both pixels in each pair deliberately have the
same luma and every pair has neutral chroma.  AYUV bits are A/Y/U/V from high
to low.  Limited-range Y=16 and Y=235 with U=V=128 map to exact clipped black
and white under the documented BT.601 conversion, avoiding coefficient tie
rounding.  The useful 32-square ranges are `0x800` and `0x1000`.  Once BGRA
destination blending is proved, AYUV alpha has an extreme follow-up:
`00eb8080` over transparent green must leave `0000ff00`, while `ffeb8080`
must replace it with opaque white.  Keep planar YV12 disabled; its plane
addresses are not supplied by these packed probes.

AI44 has a fully recovered companion-palette packet.  In
`via_chrome9_dri.so`, `stmFullUpdateHardware_invi:0x17817d..0x1781c2`
emits one aligned type-03/subtype-00 upload with count 16 and copies exactly
the 16 consecutive dwords at context offset `0x1cbc`:

```text
FE03043C 00000010 00030000 CC000000
00Y0U0V0 00Y1U1V1 00Y2U2V2 00Y3U3V3
00Y4U4V4 00Y5U5V5 00Y6U6V6 00Y7U7V7
00Y8U8V8 00Y9U9V9 00YaUaVa 00YbUbVb
00YcUcVc 00YdUdVd 00YeUeVe 00YfUfVf
```

The manual independently defines each payload as reserved/Y/U/V in bits
31:24/23:16/15:8/7:0.  Emit all 16 entries, even when a test uses only two;
partial updates would leave process-dependent colors in unused indices.
This packet is 20 dwords and both starts and ends on the required 16-byte
boundary.  In the packet above it can be inserted immediately before sampler
0 at body index 28: it occupies indices 28--47, moves the sampler to 48, and
moves every subsequent item by 20.  The exact complete REPLACE body is then
252 dwords (256 for MODULATE).  With the blend-aware closed destination group,
the totals are 256 and 260.  The palette itself does not change any type-02
register index.

The corresponding guarded fetch uses A30=`30330001`: `33` is YUV subtype
`011`, which the manual names AI44 (high nibble alpha, low nibble palette
index), and low bit one retains the Windows builder's normal BT.601-to-RGB
mode.  Use a one-byte, pitch-32, 32-square source and initialize:

```text
palette[0] = 00108080   # limited-range neutral-chroma black
palette[1] = 00eb8080   # limited-range neutral-chroma white
source f0/f1            # alpha 15, index 0/1 -> opaque black/white
```

Nearest REPLACE to XRGB must produce exact black/white.  The decisive alpha
successor uses BGRA destination A52=`99`, transparent-green target
`0000ff00`, and canonical source-over factors: byte `01` (alpha zero, white
index) must leave green, while byte `f1` must produce opaque white.  This
separately proves that the high nibble is sampled alpha rather than palette
state.  Keep AI44 unadvertised until both probes pass on hardware; IA44 uses
the same palette but needs its own A30 subtype and nibble-order test.

An sRGB sampler needs both one texture-format bit and the global five-entry
de-gamma table.  The Windows image builder at
`VTGUModeDX32A:0x101d7d85..0x101d7da2` reads a boolean at resource `+0x7c`
and replaces exactly bit 8 of the texture A30 payload; the base XRGB word
`30980000` therefore becomes `30980100`.  The Linux full-state builder at
`stmFullUpdateHardware_invi:0x177bf9..0x177c3d` emits the table verbatim:

```text
FE03043C 00000005 15030000 CC000000
41E03C05 07714434 11B36CA5 2176E565
374BD680 CC000000 CC000000 CC000000
```

Only the first nine dwords are the H3 packet; the last three words restore
16-byte alignment for the next header.  The manual defines the packed 10-bit
knots at input C=`040,080,...,3c0`; decoding the five payloads yields:

```text
C:      040 080 0c0 100 140 180 1c0 200 240 280 2c0 300 340 380 3c0
linear: 005 00f 01e 034 051 077 0a5 0db 11b 165 1b9 217 280 2f5 374
```

The first payload's high two bits select rounding mode one.  These values are
the expected sRGB-to-linear curve, rather than an arbitrary vendor color
table.  Emit all five values before enabling A30 bit 8; stale table contents
make the bit unsafe across contexts.

A bounded nearest REPLACE oracle fills an XRGB8888 source with mid-gray
`00808080`.  With the table loaded, A30=`30980000` must preserve
`00808080`, while A30=`30980100` must produce linear gray with all three
channels in `0x36..0x38` (nominal `00373737`).  Black and white endpoint
quadrants must remain exact.  This midpoint is essential: endpoints cannot
distinguish de-gamma from an ignored enable.  A BGRA-destination successor can
use A8R8G8B8 inputs `00808080` and `ff808080`; both must have the same decoded
RGB and retain alpha `00`/`ff`, proving the manual's statement that de-gamma
touches RGB only.  Keep sRGB sampler formats unadvertised until the midpoint
and alpha-preservation cases pass on silicon.

DXT1 is the first compressed format that can be tested without alpha or
palette state.  For a 32-by-32 image, the vendor's compressed layout has eight
4-row block rows, each with `align(32,4)*4/2 = 64` bytes, so the complete useful
range is `0x200` bytes.  Set A30=`30290000`, keep A20/A22 at
`20020020/22008055`, and fill every 4-by-4 block of the four 16-by-16
quadrants with these little-endian 64-bit DXT1 blocks:

```text
red   000000000000f800
green 00000000000007e0
blue  000000000000001f
white 000000000000ffff
```

Each block has RGB565 endpoint 0 equal to the named full-scale color,
endpoint 1 and all selector bits zero, so every texel selects endpoint 0.
Nearest REPLACE must again produce red/green/blue/white in XRGB8888.  Guard
the compressed range on both sides and validate source preservation.  A
future UAPI must call the 64-byte value a compressed block-row pitch and use
`ceil(height/4)` rows; treating it as 32 ordinary texel rows overestimates the
range by four and obscures later mip offsets.  Keep DXT2--DXT5 disabled until
their alpha blocks are independently tested.

The next two compressed decoders nevertheless have packet-exact, guarded
bring-up vectors.  Both use standard 4-by-4 S3TC blocks and the same four
16-by-16 constant quadrants as DXT1.  For DXT2/DXT3, set A30=`302a0000` and
prefix each eight-byte color block with an all-opaque explicit-alpha block:

```text
alpha qword       ffffffffffffffff
red color qword   000000000000f800
green color qword 00000000000007e0
blue color qword  000000000000001f
white color qword 000000000000ffff
```

For DXT4/DXT5, set A30=`302b0000`.  Use alpha endpoint zero `ff`, endpoint
one `00`, and six zero selector bytes, followed by the same color qword:

```text
alpha qword       00000000000000ff
red color qword   000000000000f800
green color qword 00000000000007e0
blue color qword  000000000000001f
white color qword 000000000000ffff
```

The qwords above are written little endian in the order shown, so each
DXT2/3/4/5 block is exactly 16 bytes.  A 32-by-32 image has eight blocks per
block row and eight block rows: compressed pitch 128 and useful range
`0x400`.  Nearest REPLACE must produce opaque red/green/blue/white.  Opaque
alpha deliberately makes the premultiplied DXT2/DXT4 API distinction
irrelevant to this first raw-decoder test.  Follow it with a nontrivial alpha
and source-over discriminator before advertising DXT3 or DXT5 alpha; do not
advertise the legacy premultiplied DXT2/DXT4 formats merely because they share
the hardware selector.

Once BGRA destination A52=`99` and canonical source-over are live, the alpha
paths have extreme-value probes with no interpolation ambiguity.  Initialize
the destination to transparent green `0000ff00`, keep the sampled color red,
and compare a transparent block with an opaque block:

| decoder | transparent block | opaque block | expected target |
|---|---|---|---|
| DXT1 RGBA, A30=`30290000` | color qword `fffffffff8000000` | color qword `000000000000f800` | green / `ffff0000` red |
| DXT3, A30=`302a0000` | alpha qword `0000000000000000`, then red color qword | alpha qword `ffffffffffffffff`, then red color qword | green / `ffff0000` red |
| DXT5, A30=`302b0000` | alpha qword `24924924924900ff`, then red color qword | alpha qword `00000000000000ff`, then red color qword | green / `ffff0000` red |

The transparent DXT1 block has endpoint zero `0000`, endpoint one `f800`,
and all two-bit selectors equal three, entering the standard transparent
fourth-code path.  The transparent DXT5 block has endpoints `ff/00` and all
sixteen three-bit selectors equal one; `0x249249249249` is that packed 48-bit
selector field.  Use RGB factors `SRC_ALPHA, INV_SRC_ALPHA` and alpha factors
`ONE, INV_SRC_ALPHA`.  These vectors distinguish the alpha decoder from the
already-proved RGB decoder while avoiding half-alpha rounding.  A52=`99` here
is destination A8R8G8B8; it is unrelated to the A30 compressed-format byte.

The one bounded depth-texture case is D24X8/D24S8 through A30 format `fe`.
The Linux UMD's `INV_D3DFmt_DDPFToDepthTX:0x1a7030` returns value four for
both private formats `2032/2033` (table `0x3f25c0`), selecting the manual's
intensity Z mode (`A30[13:11]=100`, RGB and A all receive Z).  Use its exact
complete word `30fe2000` and require nearest/no-mip filtering.  A 32-by-32
source remains four bytes per texel, pitch 128, useful size `0x1000`.  Fill
quadrants with:

```text
depth24=000000, stencil=5a -> source 0000005a -> XRGB 00000000
depth24=555555, stencil=a5 -> source 555555a5 -> XRGB 00555555
depth24=aaaaaa, stencil=3c -> source aaaaaa3c -> XRGB 00aaaaaa
depth24=ffffff, stencil=00 -> source ffffff00 -> XRGB 00ffffff
```

The repeated depth bytes avoid conversion-rounding ambiguity, while distinct
low stencil bytes prove that the sampler does not leak stencil into color.
This validates raw depth sampling only.  D16 and D32 are rejected by the
proprietary sampler-format lookup, and comparison/percentage-closer state is a
separate unknown; do not infer shadow-texture support from this oracle.

The Windows builders close one tempting but unsafe inference.  D3D helper
`VTGUModeDX32A:0x1020a960` and the byte-identical OpenGL helper
`VTGOGL32A:0x1025d890` return zero for non-depth formats.  For `2032/2033`
they follow `resource+0x2f4`, compare the resulting object's dword `+0x1c`
with eight, and return Z mode four only on equality; otherwise they return
one.  The caller shifts that result by 11 and replaces exactly A30 bits
`[13:11]` (`0x101d7f57..0x101d7f7d`, duplicated at
`0x10240a87..0x10240aad`).  Mode one is not one of the manual's documented
`000/010/100/110` component-routing encodings and there is no compare
reference or compare-function state in this helper.  It therefore must not be
renamed "PCF" or submitted as a shadow mode.  For VX900 bring-up, retain the
independently proved Linux value four and reject all other Z-mode values.

## 2D, cube, and 3D-volume addressing

Type-02 A18 controls texture shape and the address domain.  The manual's face
labels contain two errors: it labels both `[7:6]` and `[1:0]` as face 0 and
both `[17:16]` and `[15:14]` as face 5.  The Linux builder removes the
ambiguity.  `stmHWPrepareTextureRegs_invi:0x1b22f0` packs the six face domains
as follows:

| A18 bits | resource | domain encoding |
|---:|---|---|
| `1:0` | face 0 / ordinary 2D / volume | `0=SL`, `1=SF` |
| `3:2` | face 1 | same |
| `5:4` | face 2 | same |
| `7:6` | face 3 | same |
| `15:14` | face 4 | same |
| `17:16` | face 5 | same |

The packing sites are `0x1b2610..0x1b261f` for face 0 and the cube branches
at `0x1b27c5..0x1b2d38` for faces 1--5.  Value `2` is reserved system memory
and value `3` is reserved/local-local in the manual; neither should be
accepted from userspace.

Other A18 fields are exact:

| A18 field | meaning |
|---:|---|
| bit 23 | force every texture-cache lookup to miss; diagnostic only |
| bit 12 | mip table entries are offsets relative to each face's L0 base |
| `[11:8]=1` | ordinary 2D texture |
| `[11:8]=2` | 3D volume, S/T/R coordinates |
| `[11:8]=3` | cube texture |
| `[11:8]` with bit 11 set | projective class |

For ordinary 2D the Linux builder sets base-offset mode and mode 1 at
`0x1b2d10`; an SF texture therefore uses `18001101`.  For volume it selects
mode 2 at `0x1b2a90..0x1b2aa3`, giving `18001201` for SF.  A21 holds the L0
R-axis length/depth.  The manual explicitly says volume textures have no mip
filtering, so the safe exposure is one level, A30 min=max=0, until silicon
proves otherwise.

Cube setup is more informative.  The builder recognizes its cube resource
flag at `0x1b2625`, selects A18 base-offset plus mode 3 at
`0x1b263d..0x1b2647`, and limits the chain to nine total levels at
`0x1b264a..0x1b2654`.  It writes the six face-root addresses, each divided by
256, to A00--A05 at `0x1b27ec..0x1b2879`.  Helpers
`stmGetCubeFaceRoot:0xee2d0` and `stmGetCubeFaceLevelNext:0xee350` walk the
resource's face and mip links.  The subtype-10 mip table is generated from
face 0's level chain at `0x1b2678..0x1b26ad`; because all faces share the same
geometry, each entry is a byte offset from its selected face root divided by
256.

A minimal linear SF cube image packet is consequently:

```text
FE020440 0000043C 00020000 CC000000
00AAAAAA 01BBBBBB 02CCCCCC 03DDDDDD
04EEEEEE 05FFFFFF 18015355 20020020
21000001 22008055 30980000 CC000000
```

`AAAAAA..FFFFFF` are the six face L0 byte addresses shifted right by eight;
this concrete packet is 32x32 X8R8G8B8, so A20 is
`0x20000000 | (0x20 << 12) | 0x20`, A22 has the POT bit and
`log2(W)=log2(H)=5`,
and A30's format byte is `98`.  `18015355` is not a magic constant: it is
mode 3, relative-offset mode, and SF (`01`) in all six independently packed
face-domain fields.  The final `CC000000` aligns the next packet.  As with
the measured 2D oracle, the command validator must relocate every face base.

The Windows OpenGL ICD closes the public face order.  Its `TexImage2D` entry at
`VTGOGL32A:0x10148ab0` range-checks targets `8515..851a` and computes the face
index with `target - 8515` at `0x10148b7d..0x10148b99`.  The unchanged index
selects `cube->face[level]` at `0x10148c5d..0x10148c7e`, is passed to the common
surface allocator/uploader at `0x10148d60..0x10148d7b`, and again selects the
face surface at `0x10148e16..0x10148e21`.  `0x101438b0` similarly uses
`face_array[index]` and copies only surface metadata.  There is no face
permutation in any of those paths.  Because the OpenGL enums are consecutive,
the packet roots are:

| hardware face/root | OpenGL target | direction |
|---:|---:|---|
| A00 / 0 | `8515` | positive X |
| A01 / 1 | `8516` | negative X |
| A02 / 2 | `8517` | positive Y |
| A03 / 3 | `8518` | negative Y |
| A04 / 4 | `8519` | positive Z |
| A05 / 5 | `851a` | negative Z |

The upload wrapper also performs no per-face row/column transform: the face
index is used only to select a surface, while generic pixel-store and transfer
helpers receive its unmodified dimensions and client pointer.  This is strong
static evidence that the sampler implements the conventional GL cube
orientation natively.  It is not quite a silicon observation because a hidden
coordinate transform in the texture instruction remains possible.  Keep the
first asymmetric six-face test: nearest sampling and unique corner colors will
confirm the inferred orientation and expose any rotation or mirror in one
draw.  Face-order capability gating is no longer necessary; orientation
gating remains prudent until that test passes.

## Color masks and logic operation

The exact Windows color-mask builder is
`VTGUModeDX32A:0x101f9cd0`; its OpenGL copy starts at
`VTGOGL32A:0x1024d990`.  D3D stores public masks at context
`+0x1e70 + 4*RT`.  Linux `STM_ColorWriteMode_INV:0x194df0` stores them at
`+0x1dec + 4*RT` and performs the same remap.  Public bit order is
`R=1,G=2,B=4,A=8`, while A53/A5B/A63/A6B use:

| hardware bit | channel | public source bit |
|---:|---|---:|
| 3 | A | 3 |
| 2 | R | 0 |
| 1 | G | 1 |
| 0 | B | 2 |

Thus the measured A53 `53010c0f` has all four writes enabled, and exact mask
deltas are:

```text
RGBA = 53010C0F   RGB = 53010C07   R = 53010C04
G    = 53010C02   B   = 53010C01   A = 53010C08
none = 53010C00
```

The RT0 remap is now live beyond the all-channel capture: a blend-disabled
R-only successor produced the expected `00ffff00`, and a following legacy
COPY restored its full-channel blue result without recreating the context.
This validates A53 bit 2 as R and state closure across successor/legacy draws;
G, B, and stored A still retain their static/cross-driver evidence until the
full mask matrix is executed.

The upper fields must be preserved.  A53/A5B/A63/A6B `[11:8]` is the logic
operation; bit 6 enables de-gamma of destination reads; `[5:4]` controls gamma
on writes; `[3:0]` is the mask.  Windows can couple read de-gamma and sRGB
write state in this builder.  Linux context `+0x4ffc` and Windows context
`+0x6198` are internal overrides which force all channel masks to zero.

That coupling is exact in
`VTGUModeDX32A:0x101f9d44..0x101f9d85`: when RT0's sRGB boolean at context
`+0x1dc0` is nonzero, the builder selects write mode `01` in `[5:4]` and sets
read de-gamma bit 6.  The measured all-channel COPY word therefore changes
only from `53010c0f` to `53010c5f`.  Manual write mode `01` is the built-in
one-to-one linear-to-gamma-2.2 mapping; it does not use the programmable
subtype-17 table.  Preserve the COPY nibble and mask while toggling these
color-space bits.

A bounded write-only test uses blending disabled, an XRGB target cleared to
black, and nearest REPLACE from linear XRGB source `00808080`.  A53=`53010c0f`
must write `00808080`; A53=`53010c5f` must write sRGB gray with each channel
in `0xbb..0xbd` (nominal `00bcbcbc`).  Black and white remain exact and are
not sufficient discriminators.  Bit 6 is harmless here because destination
read is unused.

After source-over is live, a second test proves bit 6 rather than only write
gamma.  Initialize the target to encoded sRGB gray `00808080`, sample BGRA
white with alpha `0x80`, and use canonical source-over factors.  With
A53=`53010c5f`, the destination is first decoded to about 0.216 linear, blended
with white to about 0.610, then encoded; expect each RGB channel in
`0xcc..0xce` (nominal `0xcd`).  If destination-read de-gamma is missing but
write gamma remains enabled, the result is near `0xe1`, a wide discriminator.
Keep framebuffer-sRGB capability disabled until both tests pass, and retain
the independently required A52=`99` gate when stored destination alpha matters.

`VTGUModeDX32A:0x101fd700` converts the public 1-based D3D/GL operation.
The jump table at `0x101fd784` is independently identical to Linux
`stmConvertLogicOp:0x192b50`, whose table is at ELF `0x3f24a0`:

| public operation | hardware A53 `[11:8]` |
|---|---:|
| CLEAR | `0` |
| AND | `8` |
| AND_REVERSE | `4` |
| COPY | `c` |
| AND_INVERTED | `2` |
| NOOP | `a` |
| XOR | `6` |
| OR | `e` |
| NOR | `1` |
| EQUIV | `9` |
| INVERT | `5` |
| OR_REVERSE | `d` |
| COPY_INVERTED | `3` |
| OR_INVERTED | `b` |
| NAND | `7` |
| SET | `f` |

Windows builder `0x101fd7d0` and Linux
`STM_ColorLogicOpEnable_INV:0x196bf0` apply this nibble independently to the
four targets.  Disabling logic-op forces COPY (`c`), not zero.  Therefore an
XOR successor to the all-channel measured state changes only
`53010c0f -> 5301060f`; COPY restores `53010c0f`.  Blending and logic-op both
read the destination, so the state tracker must make them mutually exclusive
according to the API instead of enabling both hardware paths.

### Bounded logic-op lowering

Mesa 26.2 makes this unusually direct.  `st_update_blend()` stores
`ctx->Color._LogicOp` in `pipe_blend_state::logicop_func`.  The internal
`gl_logicop_mode` enum is already the four-bit Boolean truth table, numbered
`CLEAR=0, NOR=1, ... COPY=c, ... SET=f`; it is exactly the hardware A53
`[11:8]` nibble above.  There is no pipe-screen capability which suppresses
ordinary logic operations, so rejecting `logicop_enable` is a core semantic
hole rather than truthful feature gating.

The smallest UAPI extension needs only an enable flag and a normalized
four-bit operation enum; never expose A53 itself.  Kernel lowering is:

```text
if disabled: op = c                         # COPY
A53 = 53010000 | (op << 8) | hw_color_mask
A00 blend-enable bit 4 = 0
```

Reject blend-enable plus logic-op-enable even if malicious userspace supplies
both.  Keep the existing closed A38--A3F block and channel-mask lowering, and
route logic-op draws through the blend-aware successor even with full RGBA
writes.  Since current blend-state reserved dwords are being considered for
alpha test, a later size-preserving field allocation must assign the logic-op
nibble explicitly rather than overloading factor fields.

The first guarded silicon discriminator can use an XRGB target prefilled
`00112233` and a uniform RGB source `00f00ff0`:

```text
XOR:  A53=5301060f, expected covered = 00e12dc3
NOOP: A53=53010a0f, expected covered = 00112233
COPY: A53=53010c0f, expected covered = 00f00ff0
```

Run COPY immediately after XOR without recreating the context to prove state
closure, then combine XOR with R-only mask (`53010604`), expecting
`00e12233`.  Exact byte inputs avoid blend-rounding ambiguity.  Require both
guards, uncovered pixels, row padding and the normal post-test regression.

The same source/destination pair gives a compact exhaustive 16-operation
corpus.  Expected XRGB low 24 bits, indexed directly by the pipe/hardware
truth-table nibble, are:

| nibble | operation | expected |
|---:|---|---:|
| `0` | CLEAR | `000000` |
| `1` | NOR | `0ed00c` |
| `2` | AND_INVERTED | `012003` |
| `3` | COPY_INVERTED | `0ff00f` |
| `4` | AND_REVERSE | `e00dc0` |
| `5` | INVERT | `eeddcc` |
| `6` | XOR | `e12dc3` |
| `7` | NAND | `effdcf` |
| `8` | AND | `100230` |
| `9` | EQUIV | `1ed23c` |
| `a` | NOOP | `112233` |
| `b` | OR_INVERTED | `1ff23f` |
| `c` | COPY | `f00ff0` |
| `d` | OR_REVERSE | `fedffc` |
| `e` | OR | `f12ff3` |
| `f` | SET | `ffffff` |

Testing all sixteen in one guarded process is preferable to extrapolating the
truth table from XOR: every case changes only A53 `[11:8]`, so it is cheap and
also verifies that none of the nibbles aliases a hidden non-ROP mode.

## Alpha test and stencil

The alpha-reference/function builder is
`VTGUModeDX32A:0x101fb090`, byte-identical at
`VTGOGL32A:0x1024edc0`, and independently implemented as
`STM_AlphaFunc_INV:0x194ad0`.  It emits A33:

```text
A33[14:12] = hardware compare selector 0..7
A33[10:0]  = ref == 255 ? 0x400 : (ref << 2) | (ref >> 6)
```

The proprietary D3D/GL builder decrements its one-based public/internal
compare value.  The resulting hardware selector and Gallium pipe enum are
both directly NEVER through ALWAYS in their usual 0..7 order.
For example ALWAYS with reference `0x80` is `33007202`.

Linux `STM_AlphaTest_INV:0x194c40` provides an important enable detail.  Its
normal GL path sets type-01 A00 bit 20, the manual's “use RT0 alpha result for
all render targets” bit, rather than setting only A00 bit 16.  Starting from
the measured `00000011`, enable is `00100011`.  The routine suppresses the bit
for private texture formats `1028`, `102c`, and `203c..203e`; the binary does
not establish why those formats receive special policy.  Alpha-test state also
participates in the fragment-program key, so merely emitting A33 without the
matching program/state update is insufficient for a general GL driver.

Stencil is already fully recovered in
[windows-draw-path.md](windows-draw-path.md): A23/A25 hold reference,
comparison mask, and write mask; A24/A26 hold compare and fail/Z-fail/pass
operations.  `VTGUModeDX32A:0x101fc060` is byte-identical to
`VTGOGL32A:0x1024fa10`.  Stencil enable is A01 bit 4 and is accepted only for
the bound D24S8 private format `2033`.  D3D/GL op values map directly after
subtracting one:

```text
0 KEEP, 1 ZERO, 2 REPLACE, 3 INCR_SAT,
4 DECR_SAT, 5 INVERT, 6 INCR_WRAP, 7 DECR_WRAP
```

The packet-exact “always replace with 0x5a” state is:

```text
01C01510              # color/Z off, stencil on
235AFFFF 24070002     # CW: ref 5a, masks ff, ALWAYS, REPLACE on pass
255AFFFF 26070002     # CCW mirror
```

The vendor may exchange CW and CCW packs after front-face selection.  Mesa
must make that decision after applying winding and cull state, not assume CW
is permanently the API front face.

## Fog state

The fixed-function fog registers are fully reconstructable.  Windows
`VTGUModeDX32A:0x101fb350` reads packed D3DCOLOR at context `+0x1e44` and
writes the converted channels as follows; the byte-identical OpenGL copy is
at `VTGOGL32A:0x1024f080`, and Linux
`STM_FogColor_INV:0x192e00` independently agrees:

| field | channel | representation |
|---:|---|---|
| A71 `[22:12]` | R | unsigned 0..`400` |
| A71 `[10:0]` | G | unsigned 0..`400` |
| A72 `[10:0]` | B | unsigned 0..`400` |

Each API byte is divided by 255 and converted to ten fractional bits.  Pure
red is therefore `71400000,72000000`; pure green is
`71000400,72000000`; pure blue is `71000000,72000400`.

A70 supplies the source and equation:

| A70 field | meaning |
|---:|---|
| bit 7 | PS handles fog factor/operation; fixed PE fog enables must be off |
| bit 6 | use the fog-coordinate attribute |
| bit 5 | use secondary-color alpha as fog factor |
| bit 4 | select W/Z source when bit 6 is clear |
| bit 3 | select the second documented blend equation |
| `[2:0]=0` | local/no table mode |
| `[2:0]=2` | linear |
| `[2:0]=4` | exponential |
| `[2:0]=5` | exponential squared |

Windows mode builder `0x101fb560` maps its internal 0,1,2,3 to hardware
0,2,4,5.  Linux `STM_FogTableMode_INV:0x194690` maps the GL modes EXP,
EXP2,LINEAR to 4,5,2.  Linux `STM_FogEnable_INV:0x194580` and Windows
`0x101fbb70` enable all four A00 fog bits `[11:8]` together; from the measured
base state that delta is `00000011 -> 00000f11`.

Windows `0x101fb450/0x101fb4c0` and Linux
`STM_FogDensity_INV:0x192f60` place density in A78 `[20:8]` as positive
floating `[8].5`.  Windows `0x101fb630/0x101fb890` and Linux
`STM_UpdateFogRange_INV:0x193080` encode linear range:

| word | exact field |
|---:|---|
| A73 `[14:0]` | start, floating `[8].7` |
| A75 `[3:0]` | low mantissa bits of `1/(end-start)` |
| A76 `[7:0]` | IEEE-style exponent of `1/(end-start)` |
| A77 `[14:0]` | end, floating `[8].7` |

The manual's prose for A70 bit 4 contradicts itself once, while its pseudocode
and both builders agree on the actual selection.  A first Mesa driver should
prefer shader fog; fixed fog is useful as a packet discriminator but should
not be advertised until its W/Z convention and range encoding are measured.

### Smallest fixed-fog discriminator

The secondary-color-alpha source avoids every unproved W/Z and range
conversion.  Starting from the live primary-color packet, normalize COLOR1
with A03 `030052d1`, keep blending disabled, set a pure-red fog color, select
secondary alpha, and enable RT0 fog:

```text
A00 = 00000f11                    # vendor all-RT fog enable policy
A70 = 70000020                    # secondary-color alpha, local mode
A71 = 71400000                    # fog R = 1, G = 0
A72 = 72000000                    # fog B = 0
A03 = 030052d1                    # normalized packed COLOR1
```

Use constant blue primary color and run two otherwise identical guarded
triangles with D2 alpha `00` and `ff`.  The conventional equation gives red
at factor zero and blue at factor one; the opposite result identifies a
reversed factor convention without ambiguity.  First run the no-output-change
control with primary and fog both red.  These endpoint tests do not validate
LINEAR/EXP/EXP2, but they prove A00/A70 enable/source, A71/A72 channel packing,
secondary-alpha linkage, and whether the fixed PE fog path composes with the
known pixel shader.  Follow each with a normal COPY draw to prove fog state is
explicitly disabled again.

## 2D/3D, render-to-texture, and presentation ordering

There are three different coherency mechanisms and they must not be
conflated:

1. CPU cache ownership is handled by `__glDpFlushCache:0x1e9bf0` and related
   ioctls or CLFLUSH policy.  This makes CPU writes visible to the GPU and CPU
   reads observe completed GPU writes.
2. GPU texture reuse uses the type-FE A00 cache-clear pulse.  The normal
   `via3DBlit:0x1f1ce0` emits at `0x1f26b1`:

   ```text
   FE020440 0000043C FE020000 CC000000
   00000011 01000021 08000101
   ```

   `__glDpSyncFrontBufferToFakeFront:0x236940` emits the same group at
   `0x237719..0x237749` with A00 `00000010`.  Bit 0 is therefore the one-shot
   texture-cache clear; `[7:4]` is the active-texture count.  The front-buffer
   helper does not pulse in this branch; that special case must not be
   generalized to render-to-texture consumers.
3. Engine/job ordering uses the command stream and a completion event/fence.
   A cache pulse cannot make an unfinished producer complete, and a fence
   cannot publish dirty CPU cache lines.

The Linux swap implementation makes the third rule concrete.
`__glS3InvSwapBuffers:0x1f4480` first calls `hwmQueryEvent` at `0x1f4508`.
When the prior event is still pending it kicks the queued DMA at `0x1f49b9`,
then polls with bounded hang detection (`hwmHangDetect` at `0x1f4a1b` and
`hwmQueryEvent` at `0x1f4a27`) before reusing the tracked event/resource.
It then notifies the drawable through `__glDpNotifySwapBuffers` at
`0x1f4540`.

The copy-present branch emits one `sfmBlt` per clip rectangle at `0x1f4684`
or uses the 3D texture-copy fallback `via3DBlit` at `0x1f484f`.  After all
rectangles it appends `hwmSendEvent` at `0x1f4873` and calls
`cmKickoffDMA` at `0x1f4887`.  This is the exact producer -> present copy ->
event -> kickoff ordering.  The page-flip branch calls `sfmFlip` at
`0x1f4955`, exchanges front/back surface pointers at `0x1f495a..0x1f4978`,
and updates the current render target if necessary.  It does not append the
copy branch's event in this function: `sfmFlip_inv:0xeddb0` dispatches to the
chip/display flip callback (`VT3353_Flip` for 7122 at `0xeded0..0xedf09`), so
a modern KMS page flip must use DRM display/vblank completion, not pretend the
3D event is its completion signal.

`__glDpSyncFrontBufferToFakeFront` is also a packet-exact 3D interop oracle.
For the INV/H6 branches it obtains source and destination GPU bases and
formats at `0x236c3b..0x236c96`, emits destination A50--A53 at
`0x236e27..0x236e55`, source texture A00/A18/A20/A21/A22/A30 at
`0x2375d9..0x2376a9`, the cache/link group above, and then the texture sample
program and immediate quad.  This proves that a front/display resource is not
handed directly to a special copy engine in every case; the ordinary 3D
texture and render-target domains are a required fallback.

The safe modern policy is consequently:

* serialize first-generation 2D and 3D jobs on one scheduler timeline until
  T11 engine-arbitration behavior is measured;
* keep all BOs and their PTEs pinned through the producer fence;
* after render-to-texture, wait/order the producer and emit the consumer's
  normal A00 bit-0 texture pulse;
* perform CPU cache maintenance only on CPU ownership transitions;
* use a render fence for copy presents and DRM vblank/page-flip completion for
  scanout flips;
* never recycle an event slot, command ring range, BO, or PTE based only on
  command-reader position.

## Smallest additional silicon discriminators

These tests extend already measured streams without inventing packet classes:

1. **Color mask:** draw the known RGB triangle over `0x11223344`, changing
   only A53 to `53010c04`, `...02`, `...01`, and `...08`.  Exactly R, G, B,
   and A respectively may change; guards must remain intact.
2. **Logic op:** draw the same source once with COPY `53010c0f`, then with XOR
   `5301060f` over a nonuniform destination.  CPU-computed truth-table results
   must match every channel.
3. **Stencil:** run the `0x5a` replace packet above on guarded D24S8 and verify
   that only the low byte changes in covered samples.
4. **Volume:** sample two differently colored slices from a 2x2x2 SF resource
   using `18001201`, A21=`21000002`, and nearest R filtering.  Keep min=max=0.
5. **Cube:** use six faces with unique corner colors, `18015355`, one level,
   and axis-center plus off-center directions to recover public face order and
   orientation.
6. **Interop:** render red into a source, fence it, then run the measured
   texture copy with and without A00 bit 0.  The explicit-pulse path must see
   red; force-miss A18 bit 23 is diagnostic only.

All tests need SF/PTE relocations, two-sided guards, a bounded fence wait, and
full state restoration.  Cube and volume tests should come only after the
measured 2D texture and untextured regressions pass in the same boot.

## Remaining uncertainty

The exact same-format translated-SF copy path and its presentation ownership
protocol are specified separately in
[sf-2d-copy-present.md](sf-2d-copy-present.md).

The state needed for baseline fixed-function GL is now substantially closed.
The remaining gaps in this tranche are narrow and experimentally decidable:

* silicon confirmation of the statically inferred conventional per-face cube
  orientation (the public face order itself is now exact);
* actual silicon behavior of volume R interpolation and the manual's no-mip
  restriction;
* fixed fog W-versus-Z convention and numeric agreement at range endpoints;
* tiled face/slice layout and high mip levels beyond the vendor's nine-level
  cube path;
* whether finer barriers can replace producer-fence ordering between distinct
  2D/3D jobs;
* silicon confirmation of display fetch from SF, the IGA2-local interrupt
  gate, and direct-base latch timing; the exact SL automatic-flip and vblank
  fields are in [display-flip-vblank.md](display-flip-vblank.md) and remain
  KMS-only.

None of those gaps blocks linear 2D textures, ordinary color/depth/stencil,
shader fog, color masks, logic-op, or fence-serialized render-to-texture.
