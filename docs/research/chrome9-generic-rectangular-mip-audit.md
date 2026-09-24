# Chrome9 generic rectangular mip descriptor audit

## Scope and result

This audit separates the old one-texture mip ioctl from the structured
fragment-program path needed by glmark2 `refract`.  The old Mesa predicate in
`chrome9_draw.c:696..744` is intentionally limited to square POT BGRX images
no larger than 512 and at most L9.  It is called only for the non-generic
one-texture path at `:1011..1018`.  Broadening that helper would not enable
the real three-sampler refract consumer.

The generic path already has the right descriptor model for a rectangular
1600x1200 BGRA L0--L10 chain:

* Mesa serializes all physical levels at `chrome9_draw.c:1179..1220`;
* the kernel validates canonical pitches, explicit offsets, exact BO size,
  and the implicit tail at `chrome9_engine.c:7041..7136`; and
* the kernel emits exact NPOT dimensions, floor-log fields, stage-specific
  L1--L8 H3 entries, and A30--A32 at `:1589..1615,2093..2127`.

No descriptor-format special case is required for refract.  The remaining
qualification is silicon execution of the rectangular BGRA mip chain,
especially implicit L9/L10 selection.  The producer's full-size mip render
target is a separate destination-object acceptance issue and does not alter
the consumer descriptor.

## Independent proprietary evidence

All three supplied proprietary builders agree on the following image
contract.

| binary | image builder | exact dimensions / POT | A20/A22 build | explicit mip cap |
|---|---:|---:|---:|---:|
| Linux `via_chrome9_dri.so` | `stmHWPrepareTextureRegs_invi:0x1b22f0` | `0x1b2340..0x1b2365` | `0x1b24a7..0x1b25e7` | `0x1b264a..0x1b26f5`, alternate branch `0x1b2ab1..0x1b2b9d` |
| Windows D3D `VTGUModeDX32A.dll` | `0x101d7c10` | `0x101d7c8a..0x101d7cb3` | `0x101d7f83..0x101d8041` | `0x101d8112..0x101d81ec` |
| Windows OpenGL `VTGOGL32A.dll` | `0x10240740` | `0x102407ba..0x102407e3` | `0x10240ab3..0x10240b71` | `0x10240c42..0x10240d1c` |

The Linux builder reads exact width and height, writes their low twelve bits,
and converts `log2()` under x87 truncation at
`0x1b251d..0x1b25e7`.  The Windows builders read width/height from resource
`+0x144/+0x148`, test each for POT independently, keep exact low-twelve-bit
dimensions, and call the same integer-log helper for POT and NPOT images.
There is no rounding to a POT allocation.

The exact image words are therefore:

```text
A18 = 18001101
A20 = 20000000 | (height << 12) | width
A21 = 21000001
A22 = 22000000 | (both_dimensions_POT ? 00008000 : 0)
                  | (floor(log2(height)) << 4)
                  |  floor(log2(width))
A30 = 30980000 for BGRX, 30990000 for BGRA
```

The resource-level count is capped at twelve by Linux
`0x1b2316..0x1b232a` and Windows `0x101d7c66..0x101d7c72`.  Both then cap the
address loop to nine total levels, so H3 contains only L1--L8.  Linux writes
each relative address shifted by eight at `0x1b2688..0x1b26ba` or
`0x1b2b15..0x1b2b66`; Windows does the corresponding shift and location-bit
merge at `0x101d8134..0x101d81b9`.  Unused entries are cleared by Linux
`0x1b26ca..0x1b26f5` / `0x1b2b75..0x1b2b9d` and Windows
`0x101d81c7..0x101d81ec`.

The LOD builders are Linux `STM_MipMapLOD_INV:0x1b1a50`, Windows D3D
`0x101d9a40`, and Windows OpenGL `0x10242260`.  They accept resource counts
through twelve and encode:

```text
A30 = 30000000 | ((signed_bias_s5_5 & 03ff) << 12)
                  | (maximum_level << 6) | minimum_level
```

Thus L9--L11 are deliberately selectable despite having no additional H3
words.  The executed proprietary allocator `sfmCalcMipOffset_inv:0xec2e0`
places every canonical 32-bit tail level after L8 in consecutive 0x100-byte
slots.  The hardware use of that sequence remains a static inference until
the bounded fetch tests pass.

## Exact 1600x1200 current-layout vector

Mesa's private resource layout is
`[leading guard][L0][page padding][internal L0 guard][L1..Ln][final guard]`
at `chrome9_resource.c:103..160`.  For BGRA 1600x1200 L0--L10 it produces:

| level | dimensions | pitch | absolute offset | relative to L0 | useful bytes |
|---:|---:|---:|---:|---:|---:|
| L0 | 1600x1200 | `1900` | `001000` | `000000` | `753000` |
| L1 | 800x600 | `0c80` | `755000` | `754000` | `1d4c00` |
| L2 | 400x300 | `0640` | `929c00` | `928c00` | `075300` |
| L3 | 200x150 | `0320` | `99ef00` | `99df00` | `01d4c0` |
| L4 | 100x75 | `01a0` | `9bc400` | `9bb400` | `0079e0` |
| L5 | 50x37 | `00e0` | `9c3e00` | `9c2e00` | `002060` |
| L6 | 25x18 | `0080` | `9c5f00` | `9c4f00` | `000900` |
| L7 | 12x9 | `0040` | `9c6800` | `9c5800` | `000240` |
| L8 | 6x4 | `0020` | `9c6b00` | `9c5b00` | `000080` |
| L9 | 3x2 | `0020` | `9c6c00` | `9c5c00` | `000040` |
| L10 | 1x1 | `0020` | `9c6d00` | `9c5d00` | `000020` |

The complete BO is `0x9c8000` bytes.  Its descriptor is:

```text
image:   <base+1000>>8 18001101 204b0640 21000001 220000aa 30990000
H3 L1-8: 40007540 4000928c 400099df 40009bb4
          40009c2e 40009c4f 40009c58 40009c5b
sampler:  30000280 31102492 32000049 cccccccc
implicit: L9 relative 009c5c00, L10 relative 009c5d00
```

The kernel's canonical-pitch and non-overlap checks accept every row above.
Its `i > first_level + 8` rule then requires L9 and L10 to equal the previous
offset plus `0x100`, exactly matching this layout.  The final end rounds to
`0x9c7000` and the required final guard makes the exact sampled-object size
`0x9c8000`.

## Current implementation mismatches and limits

1. **The legacy helper is narrower than the generic ABI.**
   `chrome9_mip_view_supported()` rejects rectangular, NPOT, BGRA, widths
   above 512, and levels above L9.  This is correct isolation for the old
   `DRAW_TEXTURED_TRIANGLE_MIP` path, but it must not gate a generic fragment
   draw.  Current code already bypasses it for generic programs.

2. **Only physical-L0 views are actually wired.**
   Mesa rejects nonzero `first_level` in `chrome9_create_sampler_view()` at
   `chrome9_context.c:4219..4267` and again while serializing generic state at
   `chrome9_draw.c:1189`.  The kernel also requires the selected base offset
   itself to equal `PAGE_SIZE` at `chrome9_engine.c:7100..7102`.  This is
   sufficient for refract, whose view is L0--L10, but not for complete
   Gallium view-range support.  A later rebased view must keep the whole
   parent BO reserved while expressing H3 offsets relative to the selected
   base; it cannot merely delete these checks.

3. **The kernel format envelope is broader than its documentation.**
   The validator accepts mip chains for BGRX, BGRA, A8, L8, L8A8, and RGB565,
   rejecting mipmaps only for X8Z24 at `chrome9_engine.c:7054..7099`.
   Its self-test explicitly accepts one nonzero level for all four legacy
   formats at `:7139..7151,7220..7251`.  In contrast,
   `fragment-program.rst` still says mipmapped A8 is rejected.  Refract needs
   only BGRA, which is supported by both proprietary format lookup and the
   native `0x99` image byte.  Do not use refract qualification to claim the
   broader legacy-format mip envelope; either qualify those formats
   separately or narrow/document the ABI consistently.

4. **The offline self-test does not cover the dangerous boundary.**
   The existing 64x32 BGRA test stops at L1 and checks only one H3 entry and
   sampler words.  It does not check NPOT A20/A22 construction, a rectangular
   full chain, L8-to-L9 adjacency, L10/L11, or rejection of a malformed
   implicit tail.

5. **L9--L11 remain unmeasured on silicon.**
   The kernel validation and Mesa layout are mutually exact, but successful
   validation proves only that a command is bounded.  The actual hardware
   addition from L8 to the implicit tail must still be observed.

## Minimal guarded proof

### Offline validator/state self-test

Add a pure 1600x1200 BGRA L0--L10 vector using the table above.  Require:

* validation succeeds and returns object size `0x9c8000`;
* image words are `204b0640/220000aa/30990000`;
* H3 equals the eight listed words;
* sampler words equal `30000280/31102492/32000049`; and
* changing L9 or L10 by `+0x100`, making one pitch noncanonical, overlapping
  one level, populating inactive L11, or truncating the last guard is rejected.

Also add 2048x1536 L0--L11 as a validator-only ceiling case.  It must encode
`A20=20600800`, `A22=220000ab`, `A30=300002c0`, and require consecutive
L9/L10/L11 relative offsets `01001000/01001100/01001200` with exact object
size `0x01004000`.

### First live test

Do not begin with the refract shader.  Use one guarded BGRA 1600x1200 L0--L10
source, one small independent guarded BGRX destination, and the already-known
single-TEX REPLACE fragment object.  Initialize L7, L8, L9, and L10 to four
distinct opaque colors and poison every other level, all padding, and both
guards.  Submit four nearest-image/nearest-mip draws with min=max and exact
A30 values:

```text
L7 300001c7
L8 30000208
L9 30000249
L10 3000028a
```

Require the four colors in order, engine error zero, exact retirement, both
guards intact, and byte-for-byte preservation of the complete source BO.
Then run `L8 -> L9 -> L10 -> L8` without reinstalling unrelated state.  This
is the smallest test that reaches the refract-specific rectangular implicit
tail while keeping every candidate fetch inside mapped, recognizable data.

Only after that passes, test an L0/L1 trilinear footprint with
`A31=31102492`, followed by the real dense three-stage mix
`X8Z24 L0 + BGRA mip + plain BGRA L0`.  A timeout or engine error quarantines
the context and keeps the feature disabled; a wrong color with clean idle
status is a descriptor/LOD failure, not permission to force refract to L0.

