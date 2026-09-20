# Resource layout, clear, and cache-transition oracles

Status: the linear render-target and mip-address fields below are exact static
recoveries from the VX900 manual and both proprietary UMD families.  The
32-by-32 color-clear oracle is a derivative of a measured draw; the depth,
stencil, mip, and render-to-texture cases are packet-exact but have not yet
been run on silicon.  Tiled modifiers, the mip tail beyond level 8, fast Z, and
read-color/read-Z cache pulses remain deliberately unexposed.

## Linear render-target extent and stride

The manual defines the complete RT0 tuple (3D manual pp. 106--108):

| word | field | meaning |
|---:|---:|---|
| `50` | `[23:0]` | base in 256-byte units |
| `51` | `23` | tiled rather than linear |
| `51` | `22` | tiled height 16 rather than 8 |
| `51` | `[21:20]` | location: `00=SL`, `01=SF` |
| `52` | `[23:16]` | color format |
| `52` | `15` | color-extension mode |
| `52` | `12` | do not clamp PS output |
| `52` | `[9:0]` | pitch; 32-byte linear units, 256/512-byte tiled units |

There is no RT width or height in A50--A52.  A80/A81 bound the color window
and A82/A83 bound the scissor; all four coordinates have a documented range
of 0 through 2048 (manual p. 117).  The command validator must therefore
derive the actual memory extent from the resource and reject any state for
which

```text
base + (height - 1) * pitch + width * cpp > allocation_end
```

before applying the color-window/scissor bounds.  Start Mesa at 2048 by 2048,
even though the pitch field alone can describe some wider linear rows.

The ten-bit hardware field theoretically reaches `1023*32 = 32736` bytes in
linear mode.  Both vendor builders impose a lower software limit:

* Linux `STM_RenderTarget_INV:0x195f8a..0x196042` loads byte pitch from
  `surface+0x194`, masks it with `0x3fff`, shifts by five, and installs the
  low field of A52.  The alternate branch at `0x195fa4..0x195fbb` applies the
  same mask to its computed pitch.
* Windows `VTGUModeDX32A:0x101fa123..0x101fa1bf` loads byte pitch from
  `resource+0x160`, masks with `0x3fff`, shifts by five, and masks the result
  with `0x3ff`.  The OpenGL builder is byte-identical.

Thus the vendor-tested normal range is a 32-byte-aligned pitch no greater
than **16352 bytes**, not the theoretical 32736.  Use 16352 as the initial
UAPI/Mesa limit.  A 2048-wide 32-bpp target uses 8192 bytes and is well inside
it.  The old KMD verifier's `(A52 & 0x1ff) >> 5` extraction is further evidence
of the practical low range, but it is not trustworthy validation and must not
be copied.

Do not expose tiled RT modifiers yet.  Although A51/A52 give the hardware tile
height and pitch units, the resource-layout flags which make the Windows and
Linux builders choose those bits are not fully mapped, and a wrong unit can
escape a BO by hundreds of kilobytes.

## Texture size, row pitch, NPOT, and mip layout

### Established allocation rules

The manual's A20 width/height and A21 length fields are 12 bits with a stated
maximum of 2048.  A22 bit 15 distinguishes exact power-of-two dimensions;
A22 `[11:8]`, `[7:4]`, and `[3:0]` hold the POT exponents.  A22 bits 17/16
select tiled mode and a 16-texel tile height.

The executable Linux helpers add the byte layout:

* `sfmGetSurfacePitch_inv:0xebc70` returns
  `max(32, align(width*cpp, 32))` for normal linear images.  A flag-bit-15
  path uses 16-byte alignment, except formats `080e`, `080b`, and `0815`,
  which use 64.  The `00200000` block path uses
  `align(16*ceil(width/8), 32)`.
* `sfmCalcMipOffset_inv:0xec2e0` halves each dimension to one.  Normal levels
  use 32-byte-aligned row pitch and height aligned to eight.  Compressed
  levels align width to four, count four-row blocks, and align block rows to
  32 bytes.  Flag `00080000`, and the special flags case
  `flags & 003002 == 2`, align every level size to 256 bytes.
* `sfmGetSubMipAlignment_inv`, `sfmCalcNP2TilingParams_inv`, and
  `sfmCalcTileOffsetFromXYZ_inv` are stubs.  There is no missing complete NPOT
  or macrotile algorithm in this Linux binary.

For an ordinary 2D linear image, `align(row,32) * align(height,8)` is already
a multiple of 256.  For example, a contiguous 256x256 XRGB8888 chain is:

| level | dimensions | pitch | level bytes | byte offset |
|---:|---:|---:|---:|---:|
| 0 | 256x256 | 1024 | 262144 | `00000` |
| 1 | 128x128 | 512 | 65536 | `40000` |
| 2 | 64x64 | 256 | 16384 | `50000` |
| 3 | 32x32 | 128 | 4096 | `54000` |
| 4 | 16x16 | 64 | 1024 | `55000` |
| 5 | 8x8 | 32 | 256 | `55400` |
| 6 | 4x4 | 32 | 256 | `55500` |
| 7 | 2x2 | 32 | 256 | `55600` |
| 8 | 1x1 | 32 | 256 | `55700` |

These are allocator rules, not permission to infer arbitrary linear pitch at
sampling time.  There is no independent sampler pitch register.  The vendor
builder programs A20 logical width when pitch equals the canonical
`align(width*cpp,32)` row, including its normal alignment padding.  Only a
caller pitch different from that canonical row is represented as
`pitch/cpp`; the substituted effective width is also used for A22 and for
software coordinate normalization.  An initial driver should support POT,
linear images with canonical pitch first.  For NPOT, initially allow only one
level with canonical pitch.  A noncanonical imported pitch needs an explicit
coordinate/edge/wrap scheme; multiplying S by logical/effective width alone
does not preserve REPEAT or CLAMP_TO_EDGE boundaries.  Do not claim general
NPOT mipmapping merely because A22 has an NPOT bit.

The NPOT A22 exponent rule is executable, not guessed.  Linux
`stmHWPrepareTextureRegs_invi:0x1b251d..0x1b25e7` computes `log2()` for depth,
height, and width, temporarily selects x87 truncation, and packs the integer
parts into A22 `[11:8]`, `[7:4]`, and `[3:0]`.  Its preceding tests at
`0x1b2340..0x1b2365` set bit 15 only when the dimensions are exact powers of
two.  Windows performs the same three logarithms at
`VTGUModeDX32A:0x101d7fde..0x101d8041`; the resulting exponent nibbles are
present even though bit 15 tells hardware not to treat the dimensions as POT.

A bounded first NPOT test is therefore a 30x18 one-level XRGB8888 source:

```text
pitch = align(30*4,32) = 128
allocated height = align(18,8) = 24
useful range = 128*24 = 0xc00
A20 = 2001201e                 # H=18, W=30
A21 = 21000001                 # one slice
A22 = 22000044                 # NPOT; floor(log2 H/W)=4/4
A30 = 30980000
```

Initialize logical texels green, logical column 29 white, logical row 17 blue,
and row 15 yellow.  Fill padding columns 30--31 red and allocation-only rows
18--23 magenta.  With nearest plus clamp-to-edge, run three constant-coordinate
REPLACE draws:

| S / T float words | expected | catches |
|---|---|---|
| `3f000000 / 3f000000` | green | ordinary logical interior |
| `3f7bbbbc / 3f000000` | white | S=`29.5/30`; a rounded width 32 fetches red column 31 |
| `3f000000 / 3f78e38e` | blue | T=`17.5/18`; a POT/log-height 16 interpretation fetches yellow row 15 |

All poison texels, the complete 0xc00-byte allocation, and both guard pages
remain mapped.  Thus a bad logical-size interpretation produces a distinct
color rather than a fault or wedge.  Validate source and guards after every
draw.  Passing this permits only single-level canonical-pitch NPOT; it does
not establish NPOT mip offsets or noncanonical imported-pitch wrap behavior.

### The address table has eight entries, not nine

The manual's type-03/subtype-10 palette is exactly 128 words: eight entries
for levels 1 through 8 for each of sixteen texture samples (manual pp.
144--145).  Each low 24-bit value is a 256-byte-unit address/offset and bits
31:30 select the location.  The address mode is A18 bit 12:

```text
0: each table value is an independent level base
1: each table value is relative to the selected face's level-0 base
```

`stmHWPrepareTextureRegs_invi` first accepts a resource count as high as 12,
but both of its table-building branches clamp that count to **9 total
levels** at `0x1b264a..0x1b2654` and `0x1b2ab1..0x1b2abb`.  Their loops start
at level 1 and therefore write at most eight entries.  The relative-address
branch at `0x1b2695..0x1b26ad` writes
`(level_gpu - level0_gpu) >> 8`; the other branch does the same subtraction
at `0x1b2b15..0x1b2b45` when a base is available.  Unused slots have their low
24 bits cleared.

Windows `VTGUModeDX32A:0x101d7c10` independently has the same initial limit of
12 and the same cap to nine total levels at `0x101d8112..0x101d811b` and
`0x101d8658..0x101d8665`.  It writes `count-1`, at most eight, and clears the
rest.  This corrects the earlier wording that called them “nine additional”
levels.

The hardware level-control word A30 can select maximum levels 0 through 11,
and A18 uses the level-8 location for levels 8 through 11.  The likely design
is a packed tail rooted at the explicit L8 address, but neither UMD builder
encodes distinct L9--L11 addresses.  Until that derivation is measured, expose
`last_level <= 8`; a 2048 base image is still legal if the API does not require
levels 9--11.

The level-control builder itself is independently located in all three UMDs:
Linux `STM_MipMapLOD_INV:0x1b1a50`, Windows D3D
`VTGUModeDX32A:0x101d9a40`, and the byte-identical OpenGL copy
`VTGOGL32A:0x10242260`.  The two Windows copies pack the state in this exact
order at `0x101d9b7b..0x101d9ba4` / `0x1024239b..0x102423c4`:

```text
A30.low24 = ((lod_offset_s5_5 & 0x3ff) << 12)
           | ((maximum_level & 0x3f) << 6)
           |  (minimum_level & 0x3f)
```

They cap the resource level count to 12 before subtracting one for the maximum
index.  Linux performs the same `offset << 12` and `maximum << 6` packing at
`0x1b1b29..0x1b1b69`.  This is executable confirmation of the manual fields,
not merely a register-table interpretation, and closes the encoding used by
the forced-L1 oracle below.

### Full L0--L8 game-texture chain

The subtype-10 address packet is fixed at eight entries whether one or all
eight successor levels are populated.  Thus extending the two-level path to
a complete, ordinary 256x256 XRGB8888 chain changes allocation contents and
register values but no command-stream length.  Using the offsets in the
layout table above, the exact SF-relative image and address state is:

```text
FE020440 0000043C 00020000 CC000000
<base>>8 18001101 20100100 21000001
22008088 30980000 CC000000 CC000000

FE03043C 00000008 10030000 CC000000
40000400 40000500 40000540 40000550
40000554 40000555 40000556 40000557
```

The allocation useful range is `0x55800` bytes.  Every table value is the
level byte offset divided by 256 with SF location code one in bits `[31:30]`.
For a nonzero `last_level <= 8`, program sampler A30 as
`30000000 | (last_level << 6)` for unbiased L0-based sampling; the established
nearest-mip and trilinear A31 low nibbles remain 1 and 2 respectively.  The
maximum-level examples are therefore `30000040` for L1 and `30000200` for L8.

Before derivative-based filtering, validate every address entry without an
ambiguous LOD calculation.  Fill L0 through L8 respectively with opaque red,
green, blue, yellow, cyan, magenta, gray `808080`, orange `ff8000`, and white.
Issue nine nearest-filter draws while setting both minimum and maximum level
to the same `l`; the exact A30 sequence is:

```text
30000000 30000041 30000082 300000C3 30000104
30000145 30000186 300001C7 30000208
```

Each safely interior result must match its level color.  This catches every
H3 slot independently, as well as min/max field reversal, without relying on
screen-space derivatives.  All levels, row/height padding, and the complete
`0x55800` useful range must remain mapped between guard pages.  A failed table
entry can then return only one of the mapped poison colors rather than
fetching an unvalidated address.  Once this passes, use A30=`30000200` with
the already proved nearest-mip or trilinear A31 values for ordinary game
sampling.

### Packet-exact two-level mip oracle

Start with the 232-dword one-texture oracle in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  Allocate a
contiguous 32x32 XRGB8888 L0 followed by 16x16 L1:

```text
L0: offset 0000, pitch 128, allocated height 32, size 1000
L1: offset 1000, pitch  64, allocated height 16, size 0400
```

Fill L0 red and L1 green.  Keep A18 `18001100` (2D plus relative-address
mode).  Insert this exact H3 table before sampling:

```text
FE03043C 00000008 10030000 CC000000
00000010 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

`10030000` starts stage 0's eight-entry subtype-10 table; `00000010` is the
L1 byte offset `0x1000 >> 8`.  This header and eight-word emission are copied
from `stmFullUpdateHardware_invi:0x177b4c..0x177bc2`, not inferred from other
H3 packets.

That exact entry is the **SL** form paired with A18 `18001100`.  The equivalent
translated-SF texture used by the current guarded ioctl must pair A18
`18001101` with entry `40000010`: subtype-10 bits `[31:30]=01` name SF and the
low 24 bits remain the same relative offset.  A18 base-offset mode makes the
face location authoritative according to the manual, but both vendor builders
still initialize the table location consistently.  Windows does so explicitly
at `VTGUModeDX32A:0x101d8166..0x101d81b9`, selecting value one and shifting it
to bits `[31:30]` for its SF resource type; Linux installs the equivalent high
byte in the loops at `0x1b2af8..0x1b2b66`.  Do the same for complete state
closure; do not rely on the hardware ignoring a contradictory table location
merely because bit 12 is set.

In the sampler group, replace one alignment word with A30 and force the real
level zero to L1 using a +1.0 5.5 fixed-point offset:

```text
FE020440 0000043C 20020000 CC000000
30020040 31100000 32000052 CC000000
```

A30 `[21:12]=0x20` is +1.0, `[11:6]=1` makes L1 the maximum, and `[5:0]=0`
makes L0 the minimum.  A31 still says “always use texture level 0”; after the
documented A30 offset the real level zero is physical L1.  The result must be
green throughout the covered quad.  A red result isolates a level-control or
table-address failure without introducing derivative-based LOD selection.
Guard the combined 0x1400-byte useful range on both sides and validate both
the base and the L1-derived range.

For the current kernel-generated three-vertex texture body, insert the H3
subtype-10 packet immediately after the six-word texture-state group and
before the type-02 sampler group.  The packet emitter first consumes the same
two alignment dwords which previously preceded the sampler and then emits 12
dwords total, so every packet from the sampler onward moves by exactly 12.
The current exact body lengths therefore become 236 dwords for REPLACE and
240 for MODULATE.  The blend-aware bodies, already four dwords longer for
closed A38--A3F state, become 240 and 244.  Change sampler A30 from
`30000000` to `30020040`; no shader or vertex word changes.

A bounded first mip ABI need not expose raw level addresses.  Require a POT
2D image, `last_level=1`, canonical pitch at both levels, and the vendor
contiguous layout above.  For arbitrary accepted base dimensions and cpp,
derive it in the kernel as:

```text
p0 = align(width0 * cpp, 32)
s0 = p0 * align(height0, 8)
p1 = align(max(width0 >> 1, 1) * cpp, 32)
s1 = p1 * align(max(height0 >> 1, 1), 8)
level1_offset = s0
useful_size = s0 + s1
```

Do not use `p0*height0` for the mip offset when height is below or not aligned
to eight.  The formula makes every level offset 256-byte aligned without an
extra rounding step.  Derive and range-check L1 in the kernel, then emit
`40000000 | ((level1_offset >> 8) & 00ffffff)` in the first table slot for the
current SF mapping.  The 32x32 example needs 0x1400 useful bytes, rounded to
two data pages between the two guards, rather than the current single-level
three-page BO.  This prevents a userspace offset from escaping the guarded
texture mapping while proving the packet and LOD mechanism needed by Mesa.

### Combined two-texture plus mip invariant

The mip-table H3 selector is not a high-byte texture-stage selector.  The
vendor full-state loop at `stmFullUpdateHardware_invi:0x177b59..0x177b72`
forms it exactly as:

```text
10030000 | ((stage_index << 3) & 0000ffff)
```

Therefore stage 0 uses `10030000`, stage 1 uses **`10030008`**, and the
sixteen-stage sequence ends at `10030078`.  Using `11030000` for stage 1
would select a different H3 state class rather than the second texture's mip
table.

This gives an exact combined gate for a game-style mipmapped base texture
times a single-level lightmap.  Start from the 272-dword two-texture body in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md), use its RGBA-by-RGBA
ALU successor, and insert stage 0's 12-dword L1 table immediately after image
state 0 and before sampler 0.  For the current translated-SF mapping the
relevant packets are:

```text
# image 0: change A18 to SF plus relative-address mode
FE020440 0000043C 00020000 CC000000
<base0>>8 18001101 20020020 21000001
22008055 30980000 CC000000 CC000000

# stage-0 L1 table: L1 begins 0x1000 bytes after L0
FE03043C 00000008 10030000 CC000000
40000010 00000000 00000000 00000000
00000000 00000000 00000000 00000000

# sampler 0: force physical L1 for the first address test
FE020440 0000043C 20020000 CC000000
30020040 31100000 32000052 CC000000
```

Texture 1 remains the independently allocated single-level image/sampler
pair with H2 selectors `01020000`/`21020000`.  Its A18 must still name the
actual resource location (`18001101` for the current SF mapping), but it does
not need an H3 table or sampler A30 when only L0 is legal.  The stage-0 table
offset is relative to texture 0's base, never to texture 1 or the render
target.

With one table inserted, the exact combined body is **284 dwords**:

| dwords | block |
|---:|---|
| `0..15` | destination and blend |
| `16..27` | texture image 0 |
| `28..39` | texture 0 L1--L8 table |
| `40..47` | sampler 0, including LOD A30 |
| `48..59` | texture image 1 |
| `60..67` | sampler 1 |
| `68..79` | common destination/raster state |
| `80..91` | general two-texture state |
| `92..123` | two-texture H6 FVF/linkage |
| `124..131` | PS constant store |
| `132..151` | VS constants |
| `152..159` | clip |
| `160..187` | six-instruction VS |
| `188..195` | VS activation |
| `196..215` | two-texture PS controls |
| `216..223` | two TAUs |
| `224..237` | two ALUs plus terminators |
| `238..239` | vertex-header alignment |
| `240..283` | header plus four ten-dword vertices |

The RGBA-by-RGBA swizzle dword consequently moves from index 217 to index
**229** and remains `30202053`.  This index shift is a useful assertion
against accidentally patching a TAU or packet header after enabling mips.

For a single combined silicon discriminator, fill texture 0 L0 opaque black
and L1 opaque white, then fill single-level texture 1's quadrants red, green,
blue, and yellow.  The forced-L1 plus RGBA multiply must reproduce those four
texture-1 colors.  A broken mip selection produces black everywhere; the
already documented two-texture swizzle failures produce different quadrant
colors.  Both possible texture-0 levels and all texture-1 samples stay inside
their guarded allocations, so this combines the features without adding an
out-of-range fetch.

Once forced L1 passes, these are the exact ordinary sampler triples needed by
the initial game path.  They are listed in ascending sampler subaddress order
A30/A31/A32:

| API intent | A30 / A31 / A32 |
|---|---|
| nearest, single level, repeat | `30000000 31100000 32000052` |
| bilinear, single level, repeat | `30000000 31102490 32000052` |
| bilinear plus nearest mip, L0--L1, repeat | `30000040 31102491 32000052` |
| trilinear, L0--L1, repeat | `30000040 31102492 32000052` |

`A30=...0040` means zero LOD bias, maximum level 1, minimum level 0.
`A31=...2490` sets both S/T enlargement and shrinking to linear; low nibble
1 selects nearest-D mip choice and 2 selects linear-D interpolation.  The
constant `31[20:16]=16` is the maximum-anisotropy limit and does not itself
turn anisotropic filtering on.  `A32=...0052` is repeat on S/T and clamp on
unused R.  These three words fit in the existing eight-dword sampler packet
without changing any body length:

```text
FE020440 0000043C 20020000 CC000000   # or 21020000 for stage 1
30xxxxxx 31xxxxxx 32000052 CC000000
```

Keep magnification and minification independent when lowering general Gallium
state.  The compact table assumes the common game setting where both are
linear; a nearest magnification filter must clear A31 S/T enlargement fields
even when its shrinking and mip fields remain linear.

If texture 1 is also a two-level resource, insert the analogous table after
its image packet with selector `10030008`, its own relative `400000xx` L1
offset, and sampler-1 A30.  The body becomes 296 dwords and every block from
sampler 1 onward moves by another 12.  Keep that wider case gated separately;
the minimum ioquake3-style base-times-lightmap path needs only the 284-dword
stage-0-mip form.

The current kernel submits a three-vertex triangle instead of the Xorg
oracle's four-vertex quad.  Dropping only the fourth ten-dword vertex gives
these exact no-blend / blend body lengths (the blend form has the established
four-dword destination-tail delta):

```text
two single-level stages:       262 / 266 dwords
stage 0 mipmapped, stage 1 L0: 274 / 278 dwords
both stages mipmapped:         286 / 290 dwords
```

For the stage-0-only mip triangle, the RGBA modulation selector
`30202053` is body dword `229 + blend_delta`; the vertex packet header is
`240 + blend_delta` and the final three ten-dword vertices end at
`273 + blend_delta`.  Keep the two ALU terminators and the two alignment
words: eliding them does not produce a shorter equivalent packet.

Compressed mips use block rows, not the uncompressed `cpp` formula.  The
executable Linux layout at `sfmCalcMipOffset_inv:0xec2e0` takes the compressed
branch when `surface+0x20 & 0x3000`: it rounds width to four texels, computes
the block-row pitch, aligns that pitch to 32 bytes, and multiplies by
`ceil(height/4)`.  In conventional notation:

```text
block_bytes = 8 for DXT1, 16 for DXT2/3/4/5
p_l = align(ceil(width_l / 4) * block_bytes, 32)
raw_l = p_l * ceil(height_l / 4)
```

The SF-allocation branch (`surface+0x20 & 0x80000`) rounds each level's
allocation contribution to 256 bytes before advancing the next address.  A
bounded allocator should therefore use `s_l=align(raw_l,256)` for every level
that has a successor, even where a two-level experiment happens not to
observe final-level padding.

Two exact compressed forced-L1 successors follow directly.  Preserve the
same A20/A22 32-by-32 dimensions, relative SF A18=`18001101`, complete
eight-entry H3 table, and sampler words
`30020040 31100000 32000052` from the XRGB oracle:

| selector | L0 red layout | L1 green layout | H3 entry 0 | useful bytes |
|---|---|---|---:|---:|
| DXT1 texture A30=`30290000` | offset `000`, pitch 64, 8 rows, size `200` | offset `200`, pitch 32, 4 rows, raw size `080` | `40000002` | `280` |
| DXT3 texture A30=`302a0000` | offset `000`, pitch 128, 8 rows, size `400` | offset `400`, pitch 64, 4 rows, size `100` | `40000004` | `500` |
| DXT5 texture A30=`302b0000` | offset `000`, pitch 128, 8 rows, size `400` | offset `400`, pitch 64, 4 rows, size `100` | `40000004` | `500` |

Fill each level with the opaque constant-color blocks from
[fixed-function-texture-sync.md](fixed-function-texture-sync.md).  The forced
+1.0 LOD must produce green; red means the failure is in the common mip table
or level-control path, while corrupt color isolates the compressed decoder or
block-row layout.  The A30 values in the selector column are in the texture
type-02/subtype-00 group.  Sampler A30=`30020040` is a different type-02
subtype-20 register despite sharing the subaddress, and both words are
required.

After the forced-L1 test passes, two one-word derivatives separate real mip
filtering from address-table correctness:

1. Set A30=`30000040` (offset zero, max L1, min L0) and A31=`31100001`
   (nearest S/T, nearest D).  Draw the full 32-by-32 source into a 16-by-16
   destination with normalized coordinates spanning exactly zero through one.
   The two-source-texels-per-destination-pixel derivatives select LOD 1, so
   every safely interior covered sample must be green.  A red result means
   derivative LOD selection is not active even though the forced offset may
   have worked.
2. Restore a 32-by-32 destination, set A30=`30010040` (+0.5 s5.5 offset,
   max L1, min L0), and A31=`31100002` (linear D).  With same-size derivatives
   at zero, the bias selects the midpoint between red L0 and green L1.  Accept
   only `R,G in {0x7f,0x80}`, `B=0` in safely interior pixels; the one-LSB
   allowance accounts for fixed-point tie rounding.  Pure red or green means
   the D filter did not blend the two levels.

Both cases retain the same two mapped levels and guards, so even a wrong LOD
choice remains within a kernel-validated allocation.  Do not begin with an
incomplete chain or a coordinate outside `[0,1]`; that would conflate mip
selection with an out-of-range fetch.

### Guarded discriminator for the implicit L8 tail

The L9--L11 rule remains intentionally outside the first ABI, but it can be
measured without an unmapped fetch after ordinary two-level mipmapping works.
Use a complete contiguous 512-square XRGB8888 chain through L9.  Its exact
linear offsets are:

| level | dimensions | offset |
|---:|---:|---:|
| L0 | 512x512 | `000000` |
| L1 | 256x256 | `100000` |
| L2 | 128x128 | `140000` |
| L3 | 64x64 | `150000` |
| L4 | 32x32 | `154000` |
| L5 | 16x16 | `155000` |
| L6 | 8x8 | `155400` |
| L7 | 4x4 | `155500` |
| L8 | 2x2 | `155600` |
| L9 | 1x1 | `155700` |

The useful range ends at `0x155800`.  Fill L8 opaque red, L9 opaque green,
and every other level with a distinct poison color.  Program A20=`20200200`,
A22=`22008099`, A18=`18001101`, and the complete SF mip table:

```text
FE03043C 00000008 10030000 CC000000
40001000 40001400 40001500 40001540
40001550 40001554 40001555 40001556
```

Then set sampler A30=`30000249`: offset zero, maximum level 9, minimum level
9.  Keep A31 D filtering zero so “real level zero” is selected directly.  A
green result proves that the hardware derives L9 from the L8 root; red proves
clamping at the last explicit table entry.  Any poison color identifies a
different implicit packing rule.  Every plausible level and the complete
vendor-contiguous tail are mapped and guarded, so this is an address-bounded
discriminator, but no L9 capability should be exposed until it passes.

### Combined texture packet/index invariants

Border, mip, palette, de-gamma, and blend state can be composed without
guessing new indices.
Let `B=4` when the successor blend destination group is used and zero
otherwise, `M=1` when the eight-entry mip table is emitted, and `C=1` when
A35/A36 border color is emitted.  Let `P=1` when the complete 16-entry AI44
palette is emitted, and `G=1` when the five-entry texture de-gamma table is
emitted.  When multiple H3 tables are present, emit mip, palette, then
de-gamma; each block starts aligned and its padded extent ends aligned.  The
exact current-body indices are:

| invariant | body index |
|---|---:|
| destination A50 / A51 / A52 | `4 / 5 / 6` (independent of B/M/P/G/C) |
| closed blend A38--A3F | `7..14` (successor only, B=4) |
| blend A34--A37 | `15..18` (successor only, B=4) |
| texture base A00 | `20 + B` |
| texture format A30 | `25 + B` |
| mip H3 header | `28 + B` (only if M) |
| mip L1 entry | `32 + B` (only if M) |
| AI44 palette H3 header | `28 + B + 12*M` (only if P) |
| AI44 palette entries 0 / 15 | `32/47 + B + 12*M` (only if P) |
| de-gamma H3 header | `28 + B + 12*M + 20*P` (only if G) |
| de-gamma entries 0 / 4 | `32/36 + B + 12*M + 20*P` (only if G) |
| sampler H2 header | `28 + B + 12*M + 20*P + 12*G` |
| sampler A30 | `32 + B + 12*M + 20*P + 12*G` |
| sampler A31 / A32 | `33/34 + B + 12*M + 20*P + 12*G` |
| sampler A35 / A36 | `35/36 + B + 12*M + 20*P + 12*G` (only if C) |
| common A53 / A00 / A01 | `42/43/44 + B + 12*M + 20*P + 12*G + 4*C` |
| final vertex-stream H2 header, REPLACE | `196 + B + 12*M + 20*P + 12*G + 4*C` |
| final vertex-stream H2 header, MODULATE | `200 + B + 12*M + 20*P + 12*G + 4*C` |

The body length is `224 + B + 12*M + 20*P + 12*G + 4*C` for REPLACE and four
dwords more for MODULATE.  In particular, blend+mip+border is 244/248 dwords,
blend+palette is 248/252 dwords, and blend+de-gamma is 240/244 dwords.  Border adds
only two state words but costs four body dwords because the following header
must return to a 16-byte boundary.  The H3 mip block costs exactly 12 because
its header consumes the two alignment dwords that preceded the old sampler,
then adds a four-dword header and eight entries.  The H3 palette block costs
20 dwords: four header words plus all 16 entries.  The five-entry de-gamma H3
packet has nine meaningful dwords and three alignment words, hence costs 12.
Always write every table entry, zeroing unused mip addresses and initializing
every palette color, so a shorter chain or palette cannot inherit state from
an old context.  Likewise, never set texture A30 bit 8 before the complete
de-gamma table is installed.

Destination format never shifts with any optional texture feature: A52 is
always body index 6 and is `52980000 | (pitch >> 5)` for BGRX or
`52990000 | (pitch >> 5)` for BGRA.  In the closed successor, A38--A3F occupy
7--14 and A34--A37 occupy 15--18; body index 19 is the alignment word before
the texture header.  Thus selecting BGRA is a one-word change, not another
`B` delta.  The legacy non-successor has only its historical partial/staggered
constant tail and must remain BGRX-only.

## Clear paths

### What the vendor actually does

`sfmClear_inv:0xe9470` calls `cmKickoffDMA` and then `hwmWaitChipIdle` before
performing the clear.  Descriptor flags are bit 0 color, bit 1 depth, and bit
2 stencil; four color resources begin at `descriptor+0x104`, the depth
resource is at `+0x134`, depth float at `+0x148`, and stencil byte at `+0x14c`.
The ordinary color path constructs a 29-dword blit descriptor and calls
`sfmBlt` at `0xe985b`.  Ordinary depth/stencil paths also call `sfmBlt` at
`0xe9b00`, `0xe9d24`, or `0xe9fa6`.  There is no dedicated baseline clear
packet hidden at this layer.

The depth packing is exact:

| private format/path | clear representation |
|---|---|
| D16 | `INV_FloatToFix(depth,16)`, replicated into both halfwords; one at depth 1 becomes `ffffffff` |
| D24X8/D24S8 | `INV_FloatToFix(depth,24) << 8`; depth 1 is `ffffff00`; requested stencil is ORed into the low byte |
| D32 fixed | `INV_FloatToFix(depth,32)`; depth 1 is `ffffffff` |
| private `2040` D32F | raw float bits |

`stmUpdateZWClearData_inv:0x192b20` stores the packed metadata value at
`surface+0x2f4`; for `2032/2033` it clears the low byte so depth metadata does
not accidentally include stencil.  `STM_FastZClear_INV:0x195480` recognizes
the depth formats and manipulates private clear/count metadata, but depends on
capability fields and optional auxiliary allocations.  Do not enable fast Z
or coarse Z for initial clears.

### Packet-exact safe clear derivatives

For color, the measured 188-dword H6 body is already an exact 32x32 clear:
use the four vertices `(-0.5,-0.5)` through `(31.5,31.5)`, A80--A83 bounds
0..32, solid shading `23000f02`, and put the clear color in A86/A87.  Retain
A53's write masks.  This is preferable to assuming the old 2D engine can
address an SF render target; only SL/local 2D fill has been proved.

For a D16 depth-zero clear, add the four established A10--A13 words to that
body, change only A01 and A13, and use Z=0 at all four vertices:

```text
A01: 01C01540 -> 01C0150C  # color off, Z test and Z write on
A10: 10000000 | (depth_sf_va >> 8)
A11: 11004001              # 64-byte pitch, caches off, linear SF
A12: 12380000              # D16 plus fixed-point clamps
A13: 13070000              # ALWAYS
```

The covered D16 samples must become `0000`; uncovered samples and both guard
pages must retain poison.  Depth one is the next discriminator: patch each
vertex Z to `3f800000` and expect `ffff`.

For a D24S8 stencil-only clear to byte `SS`, keep Z writes/tests off and color
off, enable stencil, bind the established D24S8 surface, and use REPLACE on
both faces:

```text
A01 = 01C01510
A10 = 10000000 | (zs_sf_va >> 8)
A11 = 11008001              # 128-byte pitch for 32 pixels
A12 = 123E0000              # D24S8, linear SF
A23 = 23SSFFFF              # ref SS, compare mask ff, write mask ff
A24 = 24070002              # ALWAYS, KEEP on fail/Z-fail, REPLACE on pass
A25 = 25SSFFFF
A26 = 26070002
```

Initialize each word to a nontrivial `depth24<<8 | old_stencil`.  After the
quad, covered words must preserve all upper 24 bits and have low byte `SS`.
This state follows the byte-identical Windows D3D/OpenGL stencil builders at
`0x101fc060`/`0x1024fa10`; it is not yet a measured clear.

CPU clear remains the lowest-risk bootstrap path.  GPU clear should be enabled
one format at a time only after the guarded color, D16, and D24S8 derivatives
pass.  Fast-Z metadata must wait until ordinary depth read/write and clear are
independently solid.

## Cache and render-to-texture transitions

### The independently controlled caches

The manual exposes three unrelated controls:

| state | bit | meaning | initial policy |
|---|---:|---|---|
| type-01 A01 | 8 | enable texture cache; zero disables and clears it | leave enabled |
| type-01 A02 | 1/0 | clear/enable read-color cache | leave cache disabled |
| type-01 A11 | 11/12 | clear/enable reading-Z cache | leave cache disabled |
| type-02/FE A00 | 0 | one-shot clear texture cache | emit before texture reuse |
| per-texture A18 | 23 | always force texture-cache miss | diagnostic only |

The exact one-texture clear used by `via3DBlit:0x1f26b1` is:

```text
FE020440 0000043C FE020000 CC000000
00000011 01000021 08000101
```

A00 `11` means one active texture in `[7:4]` plus clear-cache bit 0.  The
front-buffer synchronization blit at `0x237039` emits the otherwise identical
group with A00 `10`, proving bit 0 is the only cache-clear delta.  The full
232-dword normal texture oracle already emits A00 `00000011`, with its live
texture linkage words preserved, so it already contains the explicit clear.

There is no hidden generic flush behind the public-looking helpers:

* `stmFlushPipe_inv:0x176260` returns zero without emitting anything.
* `stmUpdateTextureUsageEventTag_inv:0x1767c0` is empty.
* `STM_TextureMap_INV:0x1b2df0` binds and dirties state but contains no wait or
  cache-flush call in its binding path.
* `__glDpFlushCache:0x1e9bf0` and `__glDpMemPcieFlushCache:0x1e8dd0` are CPU
  cache/ioctl ownership operations.  They are not GPU texture-cache barriers.

Therefore the conservative first render-to-texture protocol is:

1. End the producer draw and preserve normal command-stream ordering.  Across
   jobs, wait for the producer fence and all relevant busy bits; this safely
   drains writes while finer-grained barriers are unknown.
2. Do not touch the resource from the CPU between producer and consumer.
3. Emit the consumer's normal type-02 stage/sampler state and a general
   texture group whose A00 has bit 0 set (`11` for one active texture).
4. Keep A18 bit 23 clear normally.  Use force-miss only to diagnose an
   invalidation failure.
5. Leave read-color and reading-Z caches disabled.  A02 bit 1 and A11 bit 11
   are documented clear controls, but no vendor packet ordering or pulse-back
   sequence has yet been observed; do not guess it.

CPU ownership changes and GTI page-table publication remain separate:
CLFLUSH/WB handling makes CPU memory coherent, while the bounded `SR6F.bit7`
operation publishes PTE changes.  Neither substitutes for the type-02/FE
texture-cache clear.

### Packet-exact render-to-texture discriminator

Use three guarded, linear, translated-SF XRGB8888 images at 32x32, pitch 128:
`source`, `prime_dst`, and `final_dst`.  Keep all PTEs stable for the full
sequence.

1. CPU-fill `source` blue, perform the CPU-to-GPU cache transition, then run
   the 232-dword textured body from `source` to `prime_dst`.  Its inside sample
   must be blue.  Retire the job.
2. Run the measured untextured body with `source` as RT and solid red.  Retire
   the job without CPU mapping or flushing `source`.
3. Run the textured body from `source` to `final_dst`, preserving its exact
   general-texture A00 `00000011`.  The inside sample must be red.

For diagnosis only, repeat step 3 with the following one-word alternatives:

| variant | FE A00 | A18 | interpretation |
|---|---:|---:|---|
| no explicit clear | `00000010` | `18001101` | may reproduce stale blue |
| explicit clear | `00000011` | `18001101` | required normal red result |
| forced miss | `00000010` | `18801101` | red isolates texture-cache invalidation |

The A18 values above select SF in bits 1:0; retain the source relocation in
the stage A00 word.  Require `prime_dst=blue`, `final_dst=red`, intact guards,
clean engine/error status, and the normal CPU transition only when reading the
two destination BOs after retirement.  A force-miss success plus explicit
clear failure is evidence against the assumed one-shot behavior and must stop
R2T enablement; it is not a reason to ship force-miss permanently.

## Initial implementation limits

Until the oracles above pass, enforce all of the following:

* linear color/depth/texture resources only;
* width and height at most 2048;
* render-target pitch aligned to 32 and at most 16352 bytes;
* 256-byte base addresses and complete BO-range validation;
* POT mipmapped textures with `last_level <= 8`;
* single-level NPOT textures only, with explicit row-padding semantics;
* texture-cache clear on every write-to-sample transition;
* read-color/read-Z caches, fast Z, coarse Z, and tiled modifiers disabled.

These restrictions are intentionally narrower than the advertised Windows
feature set.  They isolate address calculation and cache visibility before
performance features can turn a format or pitch mistake into a GPU wedge.
