# Chrome9 generic rectangular mip descriptor audit

## Scope and result

This audit separates the old one-texture mip ioctl from the structured
fragment-program path needed by glmark2 `refract`.  The old Mesa predicate in
`chrome9_draw.c:696..744` is intentionally limited to square POT BGRX images
no larger than 512 and at most L9.  It is called only for the non-generic
one-texture path at `:1011..1018`.  Broadening that helper would not enable
the real three-sampler refract consumer.

The distinction now also matters to the default glmark2 `texture:mipmap`
scene.  Its linked fragment shader samples the crate texture and multiplies
the result by the smooth vertex color.  Current Chrome9 accepts that shader
as one TEX plus one MUL with semantic varying mask `0x0003` and sampler mask
`0x01`, so `chrome9_use_generic_fragment_program()` is true.  The draw does
not use `DRAW_TEXTURED_TRIANGLE_MIP`, even though an older build qualified
that ioctl with `Validation: Success`.  Current Mesa serializes the complete
512-square BGRX L0--L9 resource into the structured cmd22 descriptor at
`chrome9_draw.c:1217..1259`; the old square/POT predicate and its module
parameter are not the current scene gate.

This makes one application check especially valuable after the generic mip
path is built on the target: run the exact default `texture:texture-filter=mipmap`
validation and require both glmark2's reference success and cmd22 submission.
That single run covers derivative-selected trilinear sampling through the
new descriptor, whereas repeating the legacy ioctl proof does not.  A
failure should first be classified as draw rejection, retired wrong image,
or timeout; it is not a reason to route a semantic program back through the
closed REPLACE/MODULATE object.

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

### Executed 1600x1200 allocator cross-check

The same isolated Linux 5.75 helper was executed with the exact refract
dimensions rather than extrapolating from the earlier 2048-wide cases.  The
binary remains `via_chrome9_dri.so`, SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`, and
the called entry remains `sfmCalcMipOffset_inv:0xec2e0`.  It returned offsets
relative to L0:

```text
1600x1200 L0=00000000 L1=00753000 L2=00927c00 L3=0099e800
          L4=009bc300 L5=009c4500 L6=009c6800 L7=009c7400
          L8=009c7800 L9=009c7900 L10=009c7a00 L11=009c7b00
```

This result also prevents an overly broad claim that Mesa must reproduce the
vendor allocator byte-for-byte.  The proprietary allocator charges each
level `pitch * max(8, ALIGN(height, 8))`; Mesa instead charges the logical
rows and 256-byte-aligns the next level, with an additional page guard after
L0.  Their L1--L8 offsets therefore differ for this NPOT chain.  That is
legal in the recovered state model: Linux writes each supplied relative
subresource address at `0x1b2688..0x1b26ba` or
`0x1b2b15..0x1b2b66`, and Windows obtains and writes each L1--L8 subresource
at `0x101d8134..0x101d81b9`.  H3 carries those eight explicit addresses; it
does not derive them from the vendor allocation policy.

The decisive agreement begins after the explicitly programmed L8 base:

```text
vendor:       L8=009c7800 L9=009c7900 L10=009c7a00 L11=009c7b00
current Mesa: L8=009c5b00 L9=009c5c00 L10=009c5d00
```

Both satisfy `offset(Lm) = offset(L8) + (m - 8) * 0x100`.  Therefore the
guarded oracle must program Mesa's current H3 addresses, not substitute the
vendor's absolute L1--L8 offsets, while using the vendor execution only as
evidence for the implicit post-L8 stride.  Windows independently emits no
ninth address word and leaves L9--L11 selection to A30, so it does not
introduce a competing tail address.

### Implicit-tail state invariant

There is no separate L9--L11 enable bit.  The manual gives A30[11:6] and
A30[5:0] as the complete maximum/minimum selectors through level B (11),
defines A31[3:0] only as the D/mipmap filter, and defines exactly eight H3
subtype-10 addresses per stage for L1--L8 (manual register pages 129, 130,
and 144--145).  Its A18 location procedure also groups L8, L9, L10, and L11
under `HTXnL8LOC` (page 123).  Independently, Linux caps the resource count
to twelve before capping only the H3 address loop to nine total levels, and
Windows does the same.  Neither builder changes A31 or emits another address
packet when the count exceeds nine.

For the 1600x1200 L0--L10 chain the complete tail-enabling state is therefore
only:

```text
H3 entry 7 = 40009c5b       # explicit L8 base/location
A30        = 30000280       # unbiased min 0, max 10
A31        = 31102492       # linear spatial + linear D filtering
```

Forced L10 instead uses A30=`3000028a` and nearest-D A31 low nibble one.
A31 selects how LODs are combined; it carries no level count or tail mode.
The one admissible software invariant is that, after an explicit 256-byte-
aligned L8, every reachable implicit level has canonical pitch/size, begins
exactly `0x100` after the preceding tail base, remains non-overlapping, and
the whole chain through its final guard stays pinned.  Reject `last_level>8`
if any part of that invariant fails.  Do not invent a ninth H3 word, clamp
the maximum to eight, or set an undocumented A31 bit.

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

3. **A direct nonzero-first-level request can emit a zero A20 dimension.**
   Validation minifies each active level as
   `max(texture->width >> i, 1)` / `max(texture->height >> i, 1)` at
   `chrome9_engine.c:7113..7115`.  The packet builder instead computes the
   selected dimensions with raw shifts at `:2096..2097` and writes those raw
   values to A20 at `:2104`.  For example, a direct request describing
   2048x1 with `first_level=last_level=1`, rebased so L1 starts at
   `PAGE_SIZE`, passes the dimension/layout validator as 1024x1 but emits
   A20 height zero.  Mesa's current rejection of nonzero first levels hides
   this from Gallium, but the render-node ABI must fail closed independently.
   Before enabling rebased views, make the emitter use `max(shifted, 1U)` for
   both A20 dimensions and add this exact asymmetric case to the self-test.

4. **The kernel format envelope is broader than its documentation.**
   The validator accepts mip chains for BGRX, BGRA, A8, L8, L8A8, and RGB565,
   rejecting mipmaps only for X8Z24 at `chrome9_engine.c:7054..7099`.
   Its self-test explicitly accepts one nonzero level for all four legacy
   formats at `:7139..7151,7220..7251`.  In contrast,
   `fragment-program.rst` still says mipmapped A8 is rejected.  Refract needs
   only BGRA, which is supported by both proprietary format lookup and the
   native `0x99` image byte.  Do not use refract qualification to claim the
   broader legacy-format mip envelope; either qualify those formats
   separately or narrow/document the ABI consistently.

5. **The kernel does not mirror Mesa's proven X8Z24 sampler restriction.**
   Mesa permits raw X8Z24 only with nearest minification/magnification, no
   mip filtering, and one level at `chrome9_draw.c:1009..1018`.  The kernel
   rejects a multi-level X8Z24 descriptor at
   `chrome9_engine.c:7095..7099`, but still accepts linear min/mag filtering
   for its single level because the generic filter checks at `:7076..7078`
   allow both codes for every format.  This is not merely a conservative Mesa
   policy.  Linux `STM_FilterMode_INV` tests private D24X8 format `0x2032` at
   `via_chrome9_dri.so:0x1b3418..0x1b3448` and enters a format rewrite which
   clears all four A31 spatial-filter selectors: stage byte `+0x11 &= 0x03`
   removes A31[15:10], stage word `+0x10 &= 0xfc7f` removes A31[9:7], and
   stage byte `+0x10 &= 0x8f` removes A31[6:4].  It also applies
   `+0x16 &= 0xc0` to the associated A32 controls.  Windows independently
   selects `0x2032` at `VTGUModeDX32A.dll:0x101d9709..0x101d973d`, then
   applies `state+0xc4 &= 0xffff000f` at `0x101d973d` (preserving only the
   A31 mip selector below bit 4) and `state+0xc8 &= 0xffc0ffff` at
   `0x101d9747`.  Both proprietary stacks therefore force point sampling for
   raw D24X8 instead of forwarding a requested bilinear filter.

   Refract asks for nearest, so its exact state is unaffected.  The
   render-node validator should enforce nearest minification and magnification
   for X8Z24 rather than exposing a state the production builders rewrite.

6. **The offline self-test does not cover the dangerous boundary.**
   The existing 64x32 BGRA test stops at L1 and checks only one H3 entry and
   sampler words.  It does not check NPOT A20/A22 construction, a rectangular
   full chain, L8-to-L9 adjacency, L10/L11, or rejection of a malformed
   implicit tail.

7. **L9--L11 remain unmeasured on silicon.**
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
single-TEX REPLACE fragment object.  Initialize L7 red (`ffff0000`), L8
green (`ff00ff00`), L9 blue (`ff0000ff`), and L10 white (`ffffffff`); fill
every other level and every non-guard padding interval with opaque magenta
(`ffff00ff`).  Preserve unique leading, internal-L0, and final guard
patterns, and snapshot the complete `0x9c8000` source BO before submission.
Submit four nearest-image/nearest-mip draws with min=max and exact A30 values:

```text
L7 300001c7
L8 30000208
L9 30000249
L10 3000028a
```

For each row set semantic `min_lod_s5_5 == max_lod_s5_5 == level * 32`,
zero bias, nearest image and mip filters, and clamp-to-edge.  The remaining
sampler words are therefore exactly `31100001 32000049 cccccccc`; the H3
table and image block remain unchanged between draws.  Keeping the complete
L0--L10 descriptor installed while changing only A30 makes a wrong result a
level-selection observation rather than a descriptor-layout transition.

Require red, green, blue, and white in order, engine error zero, exact
retirement, destination guards/exterior intact, and byte-for-byte
preservation of the complete source BO.  A green result for forced L9 means
clamping at the last explicit entry; magenta means another mapped address was
selected; blue is the only passing L9 result.  Then run
`L8 -> L9 -> L10 -> L8` without reinstalling unrelated state.  This is the
smallest test that reaches the refract-specific rectangular implicit tail
while keeping every candidate fetch inside mapped, recognizable data.

Only after that passes, test an L0/L1 trilinear footprint with
`A31=31102492`, followed by the real dense three-stage mix
`X8Z24 L0 + BGRA mip + plain BGRA L0`.  A timeout or engine error quarantines
the context and keeps the feature disabled; a wrong color with clean idle
status is a descriptor/LOD failure, not permission to force refract to L0.
