# Chrome9 NPOT 2-D texture capability gate

This note answers one narrow capability question: what must be true before
Gallium can set `pipe_caps::npot_textures`, and what can be tested immediately
to unblock the glamor EGL-image path without confusing a level-zero success
with the complete ARB NPOT contract.

The conclusion is favorable but conditional. Chrome9 HD has a real NPOT
descriptor path. It does not require padding the logical dimensions to a power
of two, and its ordinary normalized-coordinate sampler, wrap modes, and filter
controls are shared with POT textures. A one-level NPOT BGRX/BGRA test can use
the existing single-texture ioctl without changing its ABI. However, the
global Gallium cap also exposes mipmapped NPOT textures through the advertised
2048-pixel maximum. That requires the level table, mip filters, view ranges,
and the implicit L9--L11 tail to work. Until those gates pass, setting the cap
is a useful debug workaround for DRI's drawable-target choice, but is not a
truthful capability advertisement.

## Independent executable evidence

The descriptor builder is present in both supplied Windows user-mode drivers:

| binary | image-builder entry | exact-dimension / POT test | A20/A22 construction | mip-table cap |
|---|---:|---:|---:|---:|
| `VTGUModeDX32A.dll` | `0x101d7c10` | `0x101d7c8a..0x101d7cb3` | `0x101d7f83..0x101d8041` | `0x101d8112..0x101d8122` |
| `VTGOGL32A.dll` | `0x10240740` | `0x102407ba..0x102407e3` | `0x10240ab3..0x10240b71` | `0x10240c42..0x10240c52` |

The instruction bodies are the same apart from relocated calls and object
addresses. Each reads width and height from resource offsets `+144/+148`,
uses `(x & (x-1)) == 0` independently on both values, and sets A22 bit 15 only
when both are powers of two. It always writes the low twelve bits of width and
height to A20. It calls the same integer-log helper for all dimensions and
writes those results to A22 even when bit 15 is clear. The mip count is
initially bounded to twelve, then the explicit table loop is capped to nine
total levels, L0 through L8.

The proprietary Linux builder independently does the same work at
`stmHWPrepareTextureRegs_invi:0x1b2340..0x1b2365` and
`0x1b251d..0x1b25e7`. It temporarily selects truncation for its x87 `log2`
conversion, proving that the exponent nibbles for NPOT dimensions contain
`floor(log2(dimension))`, not a ceiling or rounded allocation size.

The Windows D3D and OpenGL sampler builders are likewise paired copies:

| state | D3D | OpenGL | recovered result |
|---|---:|---:|---|
| address modes | `0x101d9250` | `0x10241a70` | independent S/T/R codes |
| filters | `0x101d9380` | `0x10241ba0` | independent min/mag plus D/mip |
| LOD range/bias | `0x101d9a40` | `0x10242260` | A30 min/max and signed s5.5 bias |

None of these builders tests A22 bit 15 before selecting clamp, repeat,
mirror, nearest, linear, nearest-mip, or trilinear state. This is strong
static evidence that the same sampling controls apply to NPOT images. It is
not a substitute for silicon edge and LOD tests.

The VX900 manual closes the field meanings:

```text
A20[23:12] = exact L0 height, maximum 2048
A20[11:0]  = exact L0 width,  maximum 2048
A22[15]    = width and height are both powers of two
A22[7:4]   = floor(log2(L0 height))
A22[3:0]   = floor(log2(L0 width))

level l width  = max(L0 width  >> l, 1)
level l height = max(L0 height >> l, 1)
```

The final `max(...,1)` follows from the legal level range and vendor chain
construction; the manual phrases the dimension operation as a right shift.
This is the OpenGL mip-dimension rule for positive dimensions.

## Pitch and coordinate contract

There is no independent linear sampler pitch register. The executable Xorg
path `viaSet3DTexture_H5:0x2fc5a..0x2fc88` computes

```text
canonical_pitch = ALIGN(logical_width * bytes_per_texel, 32)
```

and retains the logical width in A20 when the supplied pitch equals that
value. Normal row padding is therefore supported and is not part of the
coordinate extent. Only a noncanonical pitch makes the old driver replace
A20 width by `pitch / bytes_per_texel`; that replacement also changes A22 and
the producer's coordinate normalization. It cannot preserve the logical
REPEAT or CLAMP_TO_EDGE boundary in general.

The truthful initial rule is consequently:

```text
target          = PIPE_TEXTURE_2D
dimensions      = 1..2048, exact in A20
layout          = linear, one layer
level pitch     = ALIGN(level_width * cpp, 32)
level base      = 256-byte aligned
coordinates     = ordinary normalized S/T
```

Do not scale S by `logical_width / (pitch/cpp)` for the canonical row. The
existing post-transform path already supplies normalized S/T and enables
perspective correction in type-02/FE A01. NPOT needs no different TAU
instruction or interpolation mode.

For 30x18 BGRA/BGRX, the complete level-zero image state is:

```text
pitch = ALIGN(30 * 4, 32) = 128
A18 = 18001101              # 2-D, relative bases, translated SF
A20 = 2001201e              # exact H=18, W=30
A21 = 21000001
A22 = 22000044              # NPOT, floor(log2 H/W)=4/4
A30 = 30980000              # BGRX; use 30990000 for BGRA
```

A18 bit 12 is worth retaining even for one level. The manual states that
location is taken from the face when either relative-base mode is one or the
texture is POT. With NPOT and relative-base mode zero, successor levels use
their per-level location fields instead. Existing `18001101` avoids that
state split and matches the SF-relative mip table.

## Wrap, filter, and LOD words

The sampler packet is unchanged by NPOT:

```text
A30 = 30000000 | ((lod_bias_s5_5 & 03ff) << 12)
                  | ((last_level & 3f) << 6)
                  |  (first_level & 3f)
A31 = 31100000 | (mag << 13) | (min << 10)
                  | (mag << 7) | (min << 4) | mip
A32 = 32000040 | (wrap_t << 3) | wrap_s
```

The bounded public mappings are:

| semantic | code / exact word |
|---|---|
| nearest min or mag | `0` |
| linear min or mag | `1` |
| no mip selection | `mip=0` |
| nearest mip | `mip=1` |
| linear/trilinear mip | `mip=2` |
| clamp to edge | `wrap=1`, S/T word `32000049` |
| repeat | `wrap=2`, S/T word `32000052` |
| mirrored repeat | `wrap=3`, S/T word `3200005b` |
| border | `wrap=0`, only with A35/A36 initialized |

Nearest/no-mip is A31=`31100000`; linear min/mag with no mip interpolation
is `31102490`. A31 low nibble zero means always sample the selected real level
zero; it does not mean that A30 may be omitted. Always rewrite A30--A32 when
changing images or views.

## Level-zero silicon gate available now

The current single-texture UAPI accepts NPOT dimensions, verifies canonical
pitch, emits exact A20/A22 state, and allows finite coordinates. Unlike the
two-texture validator, it has no power-of-two check. The first discriminator
therefore requires no ABI or packet-size change.

Use a 30x18 source with 128-byte pitch. Fill all logical texels with a
coordinate-derived color, column 29 white, row 17 blue, padding columns 30--31
red, and allocation padding after row 17 magenta. Keep every byte and both
guards mapped. With REPLACE, first run nearest plus clamp:

| S/T | expected purpose |
|---|---|
| `0.5 / 0.5` | ordinary logical interior |
| `(29.5/30) / 0.5` | column 29, never red row padding |
| `0.5 / (17.5/18)` | row 17, never a rounded/log2 row |

Then use a row with distinct texels and these center coordinates:

| mode | S coordinate | expected logical X |
|---|---:|---:|
| repeat | `1 + 3.5/30` | 3 |
| repeat | `-1 + 3.5/30` | 3 |
| mirror | `1 + 3.5/30` | 26 |
| mirror | `-3.5/30` | 3 |
| clamp | `1 + 3.5/30` | 29 |
| clamp | `-3.5/30` | 0 |

Finally set linear min/mag and sample `S=3/30` between contrasting texels 2
and 3. The output must be their half blend (per-channel `7f` or `80`), while
`S=(3.5/30)` must reproduce texel 3. Repeat all three phases as
`NPOT -> 32x32 POT -> NPOT` without context reset. Require idle/error zero,
immutable source/padding, and intact guards after every retirement.

Passing this establishes one-level canonical-pitch BGRX/BGRA NPOT sampling,
normalized nearest/linear coordinates, the three ordinary wrap modes on exact
NPOT boundaries, and transition closure. It is enough for glamor's usual
single-level imported-pixmap sampling, but not for the global NPOT cap.

## NPOT mip chain and packet closure

For a 30x18 chain, the vendor allocation rules give:

| level | dimensions | pitch | allocated height | bytes | offset |
|---:|---:|---:|---:|---:|---:|
| L0 | 30x18 | 128 | 24 | `0c00` | `0000` |
| L1 | 15x9 | 64 | 16 | `0400` | `0c00` |
| L2 | 7x4 | 32 | 8 | `0100` | `1000` |
| L3 | 3x2 | 32 | 8 | `0100` | `1100` |
| L4 | 1x1 | 32 | 8 | `0100` | `1200` |

The old allocator aligns each level height to eight, making every base
256-byte aligned. A modern guarded resource may place levels farther apart,
provided every offset is 256-byte aligned and within the reserved BO; the H3
table supplies actual offsets. The exact stage-zero SF-relative table is:

```text
FE03043C 00000008 10030000 CC000000
4000000c 40000010 40000011 40000012
00000000 00000000 00000000 00000000
```

All eight entries must be initialized. The first four name L1--L4 offsets
divided by 256; unused low-24 fields are zero. Pair this with
A18=`18001101`. Program A30 min/max and A31 mip filter on every draw. A kernel
successor should derive table entries from validated per-level BO offsets;
userspace must not submit raw location/address words.

The minimum NPOT mip tests are:

1. Set `min=max=level` and nearest-D for L0 through L4, with a distinct solid
   color in every level. This proves dimension halving and every address.
2. Use L0/L1 with nearest-D and a footprint selecting L1.
3. Repeat with linear-D and require the exact L0/L1 blend.
4. Exercise `first_level > 0` by rebasing a one-level view, then a multi-level
   view whose table offsets are rebuilt relative to the rebased base.
5. Run NPOT mip -> one-level POT -> NPOT mip to prove A30/table closure.

For a non-vendor resource layout, compare kernel-derived entries with
`0x40000000 | ((level_offset-level0_offset)>>8)` for every reachable level.

## Why the 2048 maximum keeps the cap gated

Gallium currently advertises `max_texture_2d_size=2048`. A complete 2048
chain has twelve levels, L0--L11. A30 can select through L11, while the H3
palette explicitly names only L1--L8. The manual selects the L8 location for
levels 8--11, and both proprietary builders accept twelve resource levels but
write only eight table entries. This proves an implicit L8-rooted tail exists;
it does not prove its exact byte packing.

The guarded L9 discriminator in
[resource-layout-clear-cache.md](resource-layout-clear-cache.md) must pass,
then be extended through L11, before the 2048-wide NPOT cap is complete. An
alternative is lowering `max_texture_2d_size` to 256, for which L0--L8 are
sufficient, but that is unsuitable for large desktop pixmaps and would not
solve the intended glamor workload.

## Exact capability decision

`caps.npot_textures=true` is truthful only after all of the following hold:

1. The level-zero gate passes for every format advertised with
   `PIPE_BIND_SAMPLER_VIEW`.
2. Nearest/linear and every accepted wrap mode pass NPOT edge tests. Sampler
   creation must not advertise modes later rejected at draw.
3. Resource creation, transfers, and views preserve canonical per-level
   pitches and checked 256-byte-aligned offsets for complete NPOT chains.
4. The kernel emits L1--L8, validates the complete BO, owns A30 min/max/bias
   and A31 mip selection, and clears unreachable entries.
5. First/last-level views, nearest mip, trilinear mip, and old/new/old state
   closure pass.
6. The implicit L9--L11 tail passes on silicon for the advertised 2048
   maximum, including at least one asymmetric NPOT chain.
7. Two-texture draws pass independent per-stage wrap/filter/mip gates.  The
   old POT-only validator is gone and the basic two-NPOT level-zero path is
   live-proven below, but that result does not cover mip selection.

Before that point, use a private debug override if necessary to prove that
the DRI target mismatch is the sole glamor blocker, and label it explicitly
non-conformant. A separate DRI/internal-drawable target-selection mechanism
would be a truthful short-term fix, but current Mesa chooses
`PIPE_TEXTURE_2D` versus `PIPE_TEXTURE_RECT` directly from this global cap in
both `dri_init_screen()` and `st_context_create()`; Chrome9 cannot express the
narrower fact with an existing pipe cap.

## Two-unit NPOT silicon result

Kernel commit `e55f146` removed the two-texture ioctl's POT-only dimension
rejection and made the per-unit A22 size word conditional exactly like the
already proven one-unit path:

```text
NPOT A22 = 22000000
POT  A22 = 22008000 | (ilog2(height) << 4) | ilog2(width)
```

The change was loaded without reboot as
`chrome9-direct-sf-npot-v2.ko`, SHA-256
`d451b138ceeffbca43eaf6e8e1f86a8942d5a5f4c276946648b2719f8db5a9e6`.
The complete Mesa smoke at nested commit `19972d4` returned zero, including:

```text
two-unit NPOT 30x18*22x14 MODULATE passed
```

MMIO engine error `040c` remained zero.  This is measured evidence that two
independent level-zero NPOT dimensions work with the existing two-sampler
MODULATE program and that clearing each unit's A22 POT/log field is correct.
It does not establish NPOT mip tables, per-stage mip selection, the L9--L11
tail, or every two-unit wrap/filter transition, so the global Gallium NPOT cap
remains behind the full checklist above.

## Confidence ledger

| fact | confidence | basis |
|---|---|---|
| exact A20 dimensions and A22 NPOT/exponents | high | manual + both Windows builders + Linux builder |
| canonical linear pitch retains logical width | high | executable Xorg setter |
| shared NPOT wrap/filter state words | high static | manual + paired Windows sampler builders |
| normalized coordinates on NPOT | high static | common sampler/TAU path; no NPOT alternate in either UMD |
| one-unit level-zero NPOT execution | high measured | 30x18 wrap/filter silicon oracle |
| two-unit level-zero NPOT execution | high measured | 30x18 by 22x14 MODULATE, `040c=0` |
| NPOT L1--L8 addressing | high static, unmeasured | manual + both UMD table builders |
| L9--L11 packing | unresolved | only L8 location/root behavior is documented |
| truthful Gallium NPOT cap today | no | mip/view/tail execution gates remain |
