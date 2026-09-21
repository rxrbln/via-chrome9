# Chrome9 two-level mip and LOD ABI oracle

This note reduces the Chrome9 HD mip state to a bounded first implementation
for the current kernel-generated one-texture triangle.  It is deliberately
limited to one linear, 32-bit, 2D texture with L0 and L1.  It does not expose
raw GPU addresses, sparse levels, cube faces, volume textures, compressed
formats, or the implicit L9--L11 tail.

The central correction is a register-namespace issue: image type-02
subaddress A18 is texture control, while image A19, A1A, and A1B are reserved.
They are **not** L1--L3 addresses.  Successor-level addresses belong to the
type-03/subtype-10 table.  In that table, stage 0 L1--L4 have HParaAdr
`00`--`03`; table addresses `18`--`1B` would mean stage 3 L1--L4.

## Evidence and confidence

The exact fields below are independently present in:

* the VX900 Chrome9 HD manual, printed pp. 122--130 and 144--145;
* Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`;
* Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`;
* the proprietary Linux texture builders already identified in
  [resource-layout-clear-cache.md](resource-layout-clear-cache.md).

The two Windows image builders are D3D `0x101d7c10` and OpenGL
`0x10240740`.  Their mip-table paths are instruction-for-instruction
equivalent after relocation.  D3D caps a resource to nine explicit levels at
`0x101d8112..0x101d811f`, writes `count-1` entries at
`0x101d8124..0x101d81c1`, and clears the unused low 24 bits at
`0x101d81c7..0x101d81ec`.  OpenGL performs the same operations at
`0x10240c42..0x10240d1c`.  The second layout branch has the matching cap at
D3D `0x101d8658..0x101d8665` and the corresponding relocated OpenGL path.

The Windows command emitters independently prove the fixed table extent.
D3D `0x10152120..0x101524dd` and OpenGL `0x101fd590..0x101fd94d` set a
`0x24`-byte record length, write selector `10030000`, advance the per-stage
source by `0x20` bytes, and iterate exactly `0x10` stages.  The record is one
four-byte selector plus eight four-byte entries.  H3 framing adds the common
four-dword header, making the submitted stage table exactly 12 dwords.

The D3D and OpenGL LOD builders at `0x101d9a40` and `0x10242260` have the
same pack sequence at `0x101d9b7b..0x101d9ba4` and
`0x1024239b..0x102423c4`.  The filter builders are D3D `0x101d9380` and
OpenGL `0x10241ba0`.  These executable paths agree with the manual fields.
The packet recipes are therefore high confidence.  Actual two-level sampling
on this silicon remains a live-test gate.

## Exact image and level state

For stage 0, the existing image packet remains:

```text
FE020440 0000043C 00020000 CC000000
<L0 SF address >> 8> 18001101 20020020 21000001
22008055 30980000 CC000000 CC000000
```

This example is a 32x32 BGRX8888 texture.  A18=`18001101` selects a 2D image,
relative level-address mode, and the already-live SF location.  A20 stores
the exact L0 height and width, not allocated or rounded dimensions.  A22 bit
15 marks a POT image and its two four-bit log2 fields are both five.  The
manual limits both A20 dimensions to 2048.

The manual explicitly labels type-02 A19--A1F reserved.  The eight explicit
successor addresses instead use this complete H3 packet:

```text
FE03043C 00000008 10030000 CC000000
40000020 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

`10030000 | (stage << 3)` selects the first of a stage's eight entries.  Entry
index `stage*8 + level-1` names L1 through L8.  Each entry's low 24 bits are
an address or offset in 256-byte units and bits `[31:30]` select the location.
For the current compatibility-guarded Mesa allocation, `40000020` means L1
is `0x2000` bytes after L0: the preserved old L0 trailing guard occupies the
intervening page.  The vendor-packed layout instead puts L1 `0x1000` after L0
and uses `40000010`.  Both are legal explicit offsets; the table word must
match the actual allocation.  Emit all eight entries even for two levels,
and write unused entries as zero.  Never reinterpret image A19/A1A/A1B as a
shorter address packet.

The manual permits sampler maximum/minimum levels 0 through 11, and the
Windows LOD builders cap a resource to 12 total levels before subtracting
one.  Only L1--L8 have explicit table entries.  L9--L11 share the L8
location/root rule and remain outside this ABI until the separate guarded
tail oracle passes.

## Exact LOD, spatial filter, and mip filter words

Sampler A30 is:

```text
A30 = 30000000
    | ((signed_lod_bias_s5_5 & 03ff) << 12)
    | ((maximum_level & 3f) << 6)
    |  (minimum_level & 3f)
```

The bias is signed ten-bit s5.5.  Useful exact L0--L1 values are:

```text
zero       30000040
+0.5       30010040
+1.0       30020040
-0.5       303f0040
-1.0       303e0040
```

Making minimum equal maximum gives derivative-independent address checks:
A30=`30000000` selects L0 and A30=`30000041` selects L1.

Sampler A31 has independent spatial and mip fields:

```text
bits 20:16  maximum anisotropy ratio, 0/1 none, up to 16
bits 15:13  S magnification: 0 nearest, 1 linear
bits 12:10  S minification:  0 nearest, 1 linear
bits  9:7   T magnification: 0 nearest, 1 linear
bits  6:4   T minification:  0 nearest, 1 linear
bits  3:0   D/mip: 0 always real level 0, 1 nearest, 2 linear, 4 dither
```

The established base keeps maximum anisotropy at 16 while selecting ordinary
non-anisotropic filters.  Exact combinations are:

| API intent | A30 | A31 | A32 repeat |
|---|---:|---:|---:|
| nearest, L0 only | `30000000` | `31100000` | `32000052` |
| bilinear, L0 only | `30000000` | `31102490` | `32000052` |
| nearest spatial, nearest mip L0--L1 | `30000040` | `31100001` | `32000052` |
| bilinear, nearest mip L0--L1 | `30000040` | `31102491` | `32000052` |
| trilinear L0--L1 | `30000040` | `31102492` | `32000052` |

Magnification and minification must remain independent in the eventual
Gallium lowering.  In particular, changing only `PIPE_TEX_FILTER_MAG` changes
A31 bits `[15:13]` and `[9:7]`; it must not alter the two shrinking fields or
the D field.  A32 wrap is orthogonal and is still written on every draw.

## Vendor-packed and current guarded allocations

The proprietary drivers use this packed formula for an uncompressed 32-bit
texture:

```text
w_l = max(width0  >> l, 1)
h_l = max(height0 >> l, 1)
pitch_l = ALIGN(w_l * 4, 32)
allocated_height_l = ALIGN(h_l, 8)
size_l = pitch_l * allocated_height_l
offset_0 = 0
offset_l = sum(size_0 ... size_(l-1))
```

Each contribution and therefore every successor offset is 256-byte aligned.
For the first oracle:

| level | dimensions | offset | pitch | allocated height | size |
|---:|---:|---:|---:|---:|---:|
| L0 | 32x32 | `0000` | `0080` | 32 | `1000` |
| L1 | 16x16 | `1000` | `0040` | 16 | `0400` |

The useful range is `0x1400` bytes.  Do not compute L1 as
`level0_pitch * logical_height` for arbitrary dimensions; the vendor layout
uses the eight-row allocation height.  The first live gate should require POT
base dimensions of at least 2x2 and at most 2048x2048, canonical pitches,
last level exactly one, and a BO large enough for the derived complete range.

The current Mesa allocator deliberately preserves the complete old L0 object,
including its trailing page guard, before adding mip levels.  With a 4 KiB
page its 32x32/16x16 object has L0 at object offset `0x1000`, the retained
guard at `0x2000`, L1 at `0x3000`, and a complete size of `0x5000`.  Relative
to L0, the table entry is therefore `40000020`.  This is the safest first ABI
profile because it does not silently change existing L0 resource layout.
See [mip-guarded-import-contract.md](mip-guarded-import-contract.md) for the
exact allocator and import boundary.

## Packet insertion and exact lengths

Insert the 12-dword H3 table immediately after the six-word stage-0 image
payload and before the stage-0 sampler packet.  For the current
`chrome9_build_textured_triangle()` body, let `B=4` when closed blend state is
present and zero otherwise.  Before mip insertion, image A00 and image A18
are at body indices `20+B` and `21+B`.  With mip state:

| word | body index |
|---|---:|
| H3 header `FE03043C` | `28+B` |
| entry count `00000008` | `29+B` |
| selector `10030000` | `30+B` |
| H3 pad | `31+B` |
| L1 entry | `32+B` |
| L8 entry | `39+B` |
| sampler H2 header | `40+B` |
| sampler selector `20020000` | `42+B` |
| sampler A30/A31/A32 | `44/45/46+B` |

All packets from the sampler onward move by exactly 12 dwords.  With
`P=4` for MODULATE and zero for REPLACE, `D=4` for the current D16 common
state and zero otherwise, the one-texture body length is:

```text
224 + B + P + D + 12
```

Consequently the exact common cases are:

| state | REPLACE | MODULATE |
|---|---:|---:|
| color only | 236 | 240 |
| blend only | 240 | 244 |
| D16 only | 240 | 244 |
| blend plus D16, when that successor is enabled | 244 | 248 |

The final immediate vertex-header index is likewise
`196+B+D+12` for REPLACE and `200+B+D+12` for MODULATE.  These formulas keep
the existing body assertions useful and catch a misplaced H3 block before a
live submission.

## Minimal successor ABI

Do not change the size or meaning of an existing ioctl and do not consume an
old `reserved` field as an address.  Add a versioned one-texture mip successor
and capability bit.  The successor may retain the current destination,
texture, blend/depth, sampler, vertex, and scissor fields, then add only:

```c
__u32 texture_last_level; /* first implementation: 1 */
__u32 mip_filter;         /* 0 none, 1 nearest, 2 linear */
__s32 lod_bias_s5_5;      /* signed ten-bit value, -512..511 */
__u32 reserved;           /* must be zero */
```

No user-provided level address, byte offset, pitch array, or A30 word is
needed.  Kernel validation and lowering should:

1. accept only the already-supported 32-bit BGRX/BGRA format and a POT 2D
   base image for the first capability;
2. validate `last_level == 1`, the existing independent nearest/linear
   spatial filters, mip filter 0--2, and signed bias range;
3. derive L0/L1 pitches, sizes, and L1 offset with the selected, capability-
   defined layout; initially use the current compatibility-guarded layout;
4. reject integer overflow and require the BO's exact derived complete size,
   including leading, retained-L0, and final guards;
5. map and retain every page covering both levels and guards until bounded
   retirement, including timeout quarantine;
6. emit A18=`18001101`, the complete eight-entry SF-relative table, and A30,
   A31, and A32 every draw;
7. advertise the capability only after the forced-L1, min=max, and transition
   tests below pass.

The `none` mip-filter value is useful for the forced-bias address oracle, but
ordinary mipmapped Gallium sampling uses nearest or linear.  Mesa should pass
semantic level/filter/bias fields.  The kernel must derive H3 words from the
agreed resource layout; userspace must not manufacture GPU offsets.  A later
switch to a vendor-packed chain must change allocator, kernel derivation, and
tests atomically.

## Bounded two-level silicon sequence

Allocate the current compatibility-guarded 32x32 plus 16x16 resource above.
Fill every
logical L0 texel opaque red and every logical L1 texel opaque green.  Fill row
and pitch padding with distinct poison colors.  Keep the destination and
texture BOs distinct, and validate both guards and all padding after each
draw.

Run these tests in order:

1. **L0 baseline:** table present, A30=`30000000`, A31=`31100000`.  A safely
   interior REPLACE triangle must be red.
2. **Address L1 without derivatives:** A30=`30020040`, A31=`31100000`.
   The +1.0 bias redefines real level zero as physical L1, so the result must
   be green.  This isolates A30 and the table from derivative calculation.
3. **Bounded min=max:** A30=`30000041`, A31=`31100001`.  The result must be
   green.  This catches reversed min/max fields while every possible fetch
   remains in the mapped two-level allocation.
4. **Derivative nearest-mip:** restore A30=`30000040`, A31=`31100001`; draw
   the full 32x32 source into 16x16 with normalized coordinates spanning
   0..1.  Safely interior pixels must be green.  Draw it 1:1 into 32x32 and
   require red.
5. **Trilinear midpoint:** draw 1:1 with A30=`30010040` and
   A31=`31102492`.  Red/green channels in safely interior pixels must each be
   `0x7f` or `0x80`, with blue zero.
6. **Transition closure:** issue forced L1, then an ordinary old single-level
   texture draw whose A30 is `30000000`, then forced L1 again.  Require
   green/red/green.  The successor always writes all eight table entries;
   the old path remains safe because its A30 makes nonzero levels unreachable.

Any timeout follows the existing wedge/quarantine path.  Do not unmap or free
the texture merely because the userspace call returned an error after packet
publication.  Only after all steps pass should Gallium expose this mip path;
none of these tests proves the implicit L9--L11 tail, sparse levels, or NPOT
mipmapping.
