# VX900 Xorg one-texture draw oracle

## Scope and provenance

This is the first complete textured successor to the silicon-proven 188-dword
untextured draw in [vp-stall-analysis.md](vp-stall-analysis.md).  It began as
an offline vendor-binary capture and has now executed successfully on the live
VX900.

The capture uses the normal `7122` state flag, setters, and emitter order for a
`PictOpSrc` copy from a linear 32x32 `x8r8g8b8` texture at GPU byte address
`07fc0000` and 128-byte pitch to the same-format target at `07fd0000`.  The
source coordinates and destination rectangle are both `(0,0)-(32,32)`.

The harness mode is `.via_xorg_probe.c` selector `0xf00d`.  It calls:

```text
state+0x178 = 1
viaInit3DState_H6
viaSet3DDestination_H5(07fd0000, 128, 20020888)
viaSet3DCompositeOperator(PictOpSrc=1, component_alpha=0)
viaSet3DDrawing(12, ffffffff, ff, ff)
viaSet3DFlags(1 texture, 0, 1, 1)
viaSet3DTexture_H5(stage=0, base=07fc0000, pitch=128, auxiliary=0,
                   width=32, height=32, format=20020888,
                   address=2, filter=2, sampler_class=0,
                   per_stage_flag=0, transform=NULL, repeat=0)
via3DEmitState_H5
via3DEmitClipRect_H5
via3DEmitVetexShader_H6
via3DEmitPixelShader_H5
via3DEmitQuad_H5
```

Those texture arguments reproduce `viaExaComposite_H5:0x29781..0x29806` and
the fields saved by `viaExaPrepareComposite_H5`; they deliberately correct the
older `0xbeef` aggregate probe's guessed address/filter and flag values.

Fresh execution of the hashed `via_drv.so` emits exactly **232 dwords**.  The
SHA-256 of the uppercase eight-hex-digit-per-line word stream is
`57f2b096bcd73ba6f5c70dade723c3f584e3b27e5bef6fb82da0c73fe5d1b823`.

## Exact 232-dword body

As with the 188-dword oracle, this excludes the outer H0 `006c=00800000`
synchronization packets, CR tail, and fence/status packet.  The two address
words are `5007fd00` for the destination and `0007fc00` for texture stage 0.

```text
# destination and blend: 0..15
FE020440 0000043C 00010000 CC000000
5007FD00 51000000 52980004 38000000
3B000000 39000000 3C000000 34000150
35000114 36000500 37000200 CC000000

# texture stage 0: 16..27
FE020440 0000043C 00020000 CC000000
0007FC00 18001100 20020020 21000001
22008055 30980000 CC000000 CC000000

# texture sampler 0: 28..35
FE020440 0000043C 20020000 CC000000
31100000 32000052 CC000000 CC000000

# common destination/raster state: 36..47
FE020440 0000043C 00010000 CC000000
860000FF 870000FF 53010C0F 00000011
01C01540 88001ED0 89000800 CC000000

# general texture/attribute state: 48..59
FE020440 0000043C FE020000 CC000000
00000011 01002521 08010101 05000000
0D810000 CC000000 CC000000 CC000000

# H6 FVF, primitive and viewport linkage: 60..91
FE020440 0000043C 00040000 CC000000
00000308 010000C2 020041D0 030052D0
04006342 1200000F 61800000 623081FF
201C5C00 23000F02 24000004 25F6CC00
28FFCC00 40800000 41400000 4200493F
43800000 44400000 4500493F 46800000
47400000 4800003F 49000000 50000000
51000000 52147FFF CC000000 CC000000

# existing PS constant store: 92..99
FE020440 0000043C 22030000 CC000000
00000000 00000000 00000000 00000000

# H6 VS float and integer constants: 100..119
FE03043C 00000008 32030400 CC000000
3F800000 3F800000 3F800000 3F800000
49400000 49400000 00000000 00000000
FE020440 0000043C 33030000 CC000000
00000000 00000000 00000000 00000000

# clip: 120..127
FE020440 0000043C 00010000 CC000000
80000020 81000020 82000020 83000020

# five-instruction one-texture VS: 128..151
FE020440 0000043C 31030000 CC000000
00380150 00000000 00000000 0008010F
0008014C 00100150 00384154 0004021F
0008414C 00000000 00000000 0008422F
0008814C 00000000 00000000 0008423F
0008C14C 00000000 00000000 0008028F

# VS valid plus last instruction index: 152..159
FE020440 0000043C 00040000 CC000000
60800004 CC000000 CC000000 CC000000

# one-texture PS controls: 160..179
FE020440 0000043C 00010000 CC000000
90018000 91000001 92010001 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 CCCCCCCC
DDDDDDDD CC000000 CC000000 CC000000

# texture-address/sample instruction: 180..187
FE020440 0000043C 21030000 CC000000
301101B0 CC000000 CC000000 CC000000

# one-texture PS ALU instruction: 188..195
FE020440 0000043C 20030000 CC000000
01C00000 301C0000 00F20105 000D083F

# TL, TR, BR, BL: 196..231; each vertex is x,y,z,w,diffuse,specular,s,t
FE020440 0000043C 00000000 CC000000
BF000000 BF000000 00000000 3F800000 FFFF0000 FFFF0000 00000000 00000000
41FC0000 BF000000 00000000 3F800000 FFFF0000 FFFF0000 3F800000 00000000
41FC0000 41FC0000 00000000 3F800000 FFFF0000 FFFF0000 3F800000 3F800000
BF000000 41FC0000 00000000 3F800000 FFFF0000 FFFF0000 00000000 3F800000
```

## Decoded deltas from the proven untextured path

- Type-02 stage state uses base in 256-byte units, dimensions `32x32`, exact
  power-of-two log2 fields `5,5`, and XRGB format `980000`.
- In type-02/FE A01 `01002521`, texture-0 bits 6:5 are `01` (perspective
  correction enabled), bits 4:2 select texture A, and bits 1:0 are `01`
  (two-dimensional S/T).  The float D0.w in each vertex is therefore the
  reciprocal-W input to interpolation, not padding; equal-W geometry cannot
  prove that a generalized structured vertex path preserves it.
- `32000052` follows the recovered formula
  `32000040 | ((filter=2)<<3) | address=2`.
- `00000308` activates D0..D3 and declares eight input dwords.  D3 is
  `04006342`: two float32 texture coordinates at input offset 6, VS input IR3.
- Subaddress 62 changes from `622081ff` to `623081ff`; the VS grows from four
  to five instructions and its last-index control changes from `60800003` to
  `60800004`.
- The PS gains one type-21 texture/sample instruction, and its type-20 ALU
  instruction changes only the texture-source fields needed for the copy.
- `via3DEmitQuad_H5` converts source pixel coordinates to normalized `s,t`.
  The four corners are exactly `(0,0)`, `(1,0)`, `(1,1)`, `(0,1)`.

## Exact two-input modulation program

The one-texture oracle above implements `texture -> oC0`, which is sufficient
for `GL_REPLACE` but not the compatibility profile's common
`GL_MODULATE(texture, primary color)` path.  The exported Xorg builder
`Modulate_H5_via_mask` (`via_drv.so:0x311d0`) provides the closest complete
vendor-scheduled two-input program.  The reproducible loader is
[`tools/re/chrome9_xorg_modulate_probe.c`](../../tools/re/chrome9_xorg_modulate_probe.c);
it supplies only the unresolved Xorg data symbols, constructs the builder's
writer object, and invokes the unmodified exported routine.  Its exact
40-dword output is:

```text
FE020440 0000043C 00010000 CC000000
90018000 91000001 92010002 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 CCCCCCCC
DDDDDDDD CC000000 CC000000 CC000000
FE020440 0000043C 21030000 CC000000
301001B0 CC000000 CC000000 CC000000
FE020440 0000043C 20030000 CC000000
0140B6D0 B0040053 00F2006D 0000007F
01C00000 301C0000 00F20105 000D083F
```

The first ALU instruction multiplies sampled `r0.wwww` by interpolated primary
color `v0` into `r1`; the second moves `r1` to `oC0`.  This is the XRender A8
mask operation, so its alpha replication is intentional.  The independently
established source fields give the direct RGBA-modulate derivative:

```text
0140B6D0 30040053 00F20005 0000007F
01C00000 301C0000 00F20105 000D083F
```

Only the first ALU instruction changes.  The high nibble `3` in its second
dword is essential: source-A's W selector is split across that nibble and the
third dword.  The old candidate `00040053 00f20005` decodes as sampled
`r0.xyzx`, not `r0.xyzw`, so it would incorrectly use the sampled red channel
as output alpha.  Two unmodified Xorg builders prove the corrected
combination independently.  `Modulate_H5_via_mask` supplies interpolated
primary color `v0.xyzw` and sampled `r0.wwww`, while
`Modulate_H5_via_src_onepix_mask` supplies sampled `r0.xyzw` with a constant
operand as `30080053 00f20005`.  Combining the former's input operand with the
latter's sample swizzle gives exactly `30040053 00f20005` without changing
any dependency or destination fields.

Use the exact controls and TAU word from the vendor builder, replace the
36-dword replace-program region
`160..195` with this 40-dword region, and retain the oracle's general texture
state, eight-dword FVF/linkage, five-instruction VS, and vertex layout.  The
resulting body is **236 dwords**.  This derivative is statically exact but not
yet live-proven; it must remain behind a validation gate.

The Linux proprietary PS 2.0 compiler independently accepts
`texld r0,t0,s0; mul oC0,r0,v0` and emits one TAU plus one ALU whose sampled
operand has the same identity swizzle:

```text
TAU 201001B0
ALU 0140B6D0 30040053 00F00005 00000820
```

Its direct-output and unscheduled control/dependency fields differ from the
executable Xorg builder, but the opcode, sources, destination, and RGBA
swizzle agree.  Thus the Xorg packet is the scheduling/control oracle and the
Linux compiler output is an independent semantic cross-check, not a
replacement packet.  The headless Windows backend probe currently uses a
minimal allocator callback which returns register zero for every allocation;
its `00040053 00f00000` result consequently must not be used as a sampled
swizzle oracle.

The smallest guarded live discriminator should first run the 236-dword body
with white vertex colors; it must reproduce the proven R/G/B/W texture.
Then use opaque-red vertex colors over the same four quadrants.  Correct RGBA
modulation yields red/black/black/red.  Leaving the XRender `wwww` swizzle in
place instead yields red in all four quadrants, making the error immediately
visible without changing addresses, formats, or sampler state.

## Exact two-texture source-times-mask program

The exported `Modulate_H5_via_mask2` builder (`via_drv.so:0x32c50`) is the
bounded two-texture successor.  Its name is important: this is not a generic
RGBA-times-RGBA combine.  The first texture supplies RGBA and only the second
texture's alpha is replicated across the multiply.  Static ALU decoding gives
the exact operation

```text
r1.xyzw = r0.xyzw * r2.wwww + 0
oC0.xyzw = r1.xyzw
```

`via3DEmitPixelShader_H5:0x33915..0x339b7` selects this builder only when the
active-texture count at state offset `38` is two and the composite selector at
offset `100` is three.  Its exact pixel-program packets are:

```text
# controls: 20 dwords including alignment
FE020440 0000043C 00010000 CC000000
90018000 91000003 92020002 93000002
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 CCCCCCCC
DDDDDDDD CC000000 CC000000 CC000000

# two texture-address/sample instructions
FE020440 0000043C 21030000 CC000000
301001B0 311211B1 CCCCCCCC DDDDDDDD

# multiply and output move; the following caller supplies two alignment pads
FE020440 0000043C 20030000 CC000000
0140B6D0 302026DB 00F20005 0000007F
01C00000 301C0000 00F20105 000D083F
CCCCCCCC DDDDDDDD
```

The first TAU writes sampled stage 0 to `r0`; the second writes sampled stage
1 to `r2`.  The first ALU source fields independently decode as identity
`r0.xyzw`, literal zero, and `r2.wwww`, with full `r1` writemask.  This avoids
the tempting but incorrect interpretation of the program as color-by-color
modulation.

Starting from the proven 232-dword body, configure a second 32x32 texture and
insert its ordinary image and sampler packets immediately after sampler 0:

```text
FE020440 0000043C 01020000 CC000000
0007FB00 18001100 20020020 21000001
22008055 30990000 CC000000 CC000000

FE020440 0000043C 21020000 CC000000
31100000 32000052 CC000000 CC000000
```

Here `07fb0000` is only an illustrative, 256-byte-aligned mask allocation;
the relocation must use the actual pinned address.  `30990000` selects
A8R8G8B8 for a real mask alpha channel.  Stage 1 differs from stage 0 in both
H2 selectors: image type `01020000` and sampler type `21020000`.

The shared general-texture group becomes exactly:

```text
FE020440 0000043C FE020000 CC000000
00000021 01002521 08010102 05000000
0D810000 CC000000 CC000000 CC000000
```

This closes a useful packet invariant.  `via3DEmitState_H5:0x2f287..0x2f2b9`
forms A00 as `((active_count << 4) & fe) | 1` and A08 as
`08010100 | (active_count & 00fefeff)`.  Therefore the two-texture values are
`A00=00000021` and **`A08=08010102`**.  A08's low byte is the count; it is not
an active-stage bitmask, so `08010103` is wrong.

The H6 FVF/linkage group remains 32 dwords but consumes the former second
trailing pad with a D4 declaration:

```text
FE020440 0000043C 00040000 CC000000
0000040A 010000C2 020041D0 030052D0
04006342 05008442 1200001F 61800000
624081FF 201C5C00 23000F02 24000004
25F6CC00 28FFCC00 40800000 41400000
4200493F 43800000 44400000 4500493F
46800000 47400000 4800003F 49000000
50000000 51000000 52147FFF CC000000
```

`0000040a` declares ten input dwords.  D3 at offset 6 carries `s0,t0`, and D4
at offset 8 carries `s1,t1`.  The packed D1/D2 declarations retain the
vendor's exact `d0` low bit here because this mask program does not consume
either packed vertex color.  A driver that feeds packed primary color to an
ALU must use the separately live-proven normalized `d1` declaration.

The vertex program gains one instruction and its activation last-index becomes
five:

```text
FE020440 0000043C 31030000 CC000000
00380150 00000000 00000000 0008010F
0008014C 00100150 00384154 0004021F
0008414C 00000000 00000000 0008422F
0008814C 00000000 00000000 0008423F
0008C14C 00000000 00000000 0008028F
0009014C 00000000 00000000 0008029F

FE020440 0000043C 00040000 CC000000
60800005 CC000000 CC000000 CC000000
```

Each streamed vertex is consequently ten dwords:

```text
x, y, z, w, diffuse, specular, s0, t0, s1, t1
```

With the otherwise unchanged destination, common state, constants, and clip,
the exact body index map is:

| dwords | block |
|---:|---|
| `0..15` | destination and blend |
| `16..27` | texture image 0 |
| `28..35` | sampler 0 |
| `36..47` | texture image 1 |
| `48..55` | sampler 1 |
| `56..67` | common destination/raster state |
| `68..79` | general texture/attribute state |
| `80..111` | H6 FVF, primitive, viewport linkage |
| `112..119` | existing PS constant store |
| `120..139` | H6 VS constants |
| `140..147` | clip |
| `148..175` | six-instruction VS |
| `176..183` | VS activation |
| `184..203` | PS controls |
| `204..211` | two TAUs |
| `212..225` | two ALUs plus `CC/DD` terminators |
| `226..227` | alignment before vertex H2 |
| `228..271` | vertex header plus four 10-dword vertices |

Thus the statically exact body is **272 dwords**.  Closed source-over blend
state, if added by a successor path, is a separate four-dword destination
delta and must not be confused with this 272-dword Xorg body.

The current kernel draw path streams one triangle rather than Xorg's
four-vertex rectangle.  Its silicon-safe two-texture translation keeps every
block and index above through the vertex header at 228, then supplies three
ten-dword vertices instead of four.  The exact triangle body is therefore
**262 dwords**, or **266** with the closed four-dword blend destination delta.
Do not drop the pixel ALU's `CCCCCCCC/DDDDDDDD` terminators: they occupy body
224/225 (plus the blend delta), followed by the two alignment words at
226/227, before the vertex H2 header.  The RGBA-swizzle candidate below is
body dword `217 + blend_delta`; a later stage-0 mip table moves it to
`229 + blend_delta`.  These assertions translate the vendor oracle directly
to the existing three-vertex builder without treating the fourth rectangle
vertex as required hardware state.

A guard-safe live discriminator uses separate, page-backed 32x32 linear
textures and reads only interior pixels away from quadrant boundaries.  Fill
stage 0's TL/TR/BL/BR quadrants with opaque red, green, blue, and white.  Fill
the corresponding A8R8G8B8 stage-1 quadrants with alpha `00,ff,80,40` and RGB
poison `ff00ff`.  Give D3 and D4 the same normalized coordinates.  With an
XRGB target the expected interiors are respectively:

```text
00000000  0000FF00  00000080  00404040
```

This distinguishes a missing second fetch, color-channel multiplication, the
wrong `r2` destination, and a broken D4 vertex stride without any out-of-range
sample.  Keep the target and both textures in distinct guarded allocations,
run the normal texture-cache invalidation, and leave this feature gated until
the full vector and guards pass on silicon.

### One-word RGBA-by-RGBA successor for game lightmaps

The alpha-mask program is not enough for fixed-function `GL_MODULATE` across
two texture units: ioquake3-style base-texture times RGB lightmap needs all
four components of stage 1.  The smallest scheduled derivative changes only
the first ALU's source-C swizzle at body dword 217:

```text
# vendor alpha-mask instruction
0140B6D0 302026DB 00F20005 0000007F   # r0.xyzw * r2.wwww

# RGBA-by-RGBA candidate
0140B6D0 30202053 00F20005 0000007F   # r0.xyzw * r2.xyzw
```

All opcode, destination, dependency, register-allocation, and scheduling bits
are preserved.  Source C occupies dword 1 bits `[20:0]`; its exact identity
swizzle is low 12 bits `053` (`x=0,y=1,z=2,w=3`), while replicated W is
`6db`.  The second, output-MOV instruction and every TAU/control word remain
unchanged.  The complete body therefore remains 272 dwords.

This is a tightly bounded synthesis rather than an unmodified vendor shader,
but it has two independent binary cross-checks.  Calling
`Modulate_H5_src_comp_Aa_Ca2` with mode 6 emits the scheduled instruction

```text
0140B6D0 30202013 00F20005 0000007F   # r0.xyzw * r2.xxzw
```

which proves the same physical `r2`, dependencies, and scheduling with a
different component-alpha swizzle.  Separately, the Linux proprietary PS 2.0
compiler accepts

```text
texld r0,t0,s0
texld r1,t1,s1
mul   r2,r0,r1
mov   oC0,r2
```

and emits two TAUs and the expected unscheduled MUL/MOV pair:

```text
201001B0 211FF1B1
0140B6D0 3003F000 00F00005 000003E0
01C00530 001C0053 00F03F00 000D0820
```

Those compiler words contain virtual allocation ids and cannot replace the
Xorg-scheduled packet directly.  They do independently establish the two
identity texture operands and operation semantic.

The live discriminator should use XRGB for both textures so alpha is exactly
one and cannot hide an RGB swizzle error.  In TL/TR/BL/BR quadrants use:

| quadrant | stage 0 | stage 1 | expected RGBA multiply |
|---|---|---|---|
| TL | red | red | red |
| TR | green | red | black |
| BL | blue | blue | blue |
| BR | white | yellow | yellow |

The old `wwww` program instead returns red/green/blue/white.  The vendor
component-alpha `xxzw` program specifically returns green in TR, so the same
vector distinguishes both known wrong swizzles.  Read safely interior pixels,
retain page guards around both sources and the target, and gate the RGBA
derivative until the exact 272-dword body, output colors, and all guards pass.

### Result-scale x2 derivative for overbright lightmaps

The same scheduled MUL has an independently proved native destination-scale
field.  Dword 3 bits `[15:13]` are the legacy result-scale code, and code 1
means multiply by two.  Consequently the minimum `GL_RGB_SCALE=2` derivative
of the RGBA program changes one word and no packet lengths:

```text
# ordinary RGBA modulation
0140B6D0 30202053 00F20005 0000007F

# RGBA modulation, destination result x2
0140B6D0 30202053 00F20005 0000207F
```

This is compiler-field-backed but not yet silicon-validated in this exact
scheduled instruction.  Both recovered proprietary compilers map D3D
destination-shift code 1 to native result-scale code 1; Linux compiler corpus
rows independently show the dword-3 delta `000d0020 -> 000d2020` on MOV.
No opcode, swizzle, dependency, saturation, destination, or subsequent MOV
changes.  The word is body dword 219 in the Xorg quad and the current kernel
triangle (`219 + blend_delta`), or `231 + blend_delta` after a stage-0 mip
table.  The earlier source-C swizzle word remains at 217/229 respectively.

A guard-safe discriminator makes stage 0 opaque gray `40,80,c0,ff` in the
four quadrants and stage 1 opaque white throughout.  On an XRGB target, x2
modulation must produce gray `80,ff,ff,ff`; the unscaled program produces
`40,80,c0,ff`.  Sample quadrant interiors with nearest filtering and retain
the two source guards and target guards.  Because this one-instruction form
scales alpha together with RGB, initially expose it only for the opaque
lightmap path; exact independent `GL_ALPHA_SCALE` lowering needs a separate
two-write shader before it can be advertised generally.

## Exact texture-image state

The image block is not merely consistent with the manual: its construction is
independently visible in both Windows user-mode drivers.  The Direct3D routine
`VTGUModeDX32A.dll:0x101d7c10` and the byte-identical OpenGL copy at
`VTGOGL32A.dll:0x10240740` use `state + 0x290 + stage * 0xc4`.  Within that
record, offsets `00,60,80,84,88,c0` are hardware subaddresses
`00,18,20,21,22,30`.  Both routines call their texture-format lookup and write
the returned byte to record offset `c2`, exactly bits 23:16 of subaddress 30.

For the oracle, the six words decode as follows:

| word | exact fields | oracle meaning |
|---:|---|---|
| `0007fc00` | `00[23:0] = address[31:8]` | source byte address `07fc0000`; therefore 256-byte address units |
| `18001100` | bit 12 base-address-offset mode; bits 11:8=`1`; all face/location fields zero | ordinary 2D texture, base-offset mode enabled, no cache-miss force |
| `20020020` | height `[23:12]=0x20`; width `[11:0]=0x20` | 32 by 32 texels |
| `21000001` | volume length/depth `[11:0]=1` | one slice |
| `22008055` | tiled bit 17=`0`; tile-height bit 16=`0`; exact-power-of-two bit 15=`1`; log2 length/height/width=`0/5/5` | linear, power-of-two, 1x32x32 |
| `30980000` | format `[23:16]=98`; other controls zero | X8R8G8B8/XRGB8888 texture |

The Windows builder takes nominal width, height, and depth from resource
offsets `+144,+148,+14c`, limits the mip count to 12, fills subaddress 20 at
`0x101d7f83`, subaddress 21 at `0x101d7fb2`, and the power-of-two/log2 fields
at `0x101d7fd3..0x101d8041`.  It clears the lower 24 address bits at
`0x101d7f72` because allocation relocation patches them at command-build time.
The relocation setup at `0x10151d10..0x10151d89` associates each bound stage
allocation with the command-buffer copy of `state+0x290+stage*0xc4`; the
resulting hardware value is allocation byte address shifted right by eight.

There is no independent linear-pitch word in this group.  The Linux setter
first computes canonical pitch as
`align(logical_width * bits_per_pixel, 256) / 8`.  When the caller pitch
matches, subaddress-20 width remains logical width even if the row contains
normal alignment padding.  Only a different caller pitch is represented as
`pitch / bytes_per_texel`, and subaddress 22 then uses the same substituted
effective width for its log2 field.  The oracle is unambiguous because its
canonical pitch is `128` and its logical width is `32`.

For mipmapped resources, the Windows builder caps the encoded chain at nine
total levels: L0 plus at most eight table entries for L1--L8.  It obtains each
sublevel's allocation address at `0x101d8134..0x101d8152` and stores
`address >> 8`, carrying location/layout selectors in the high bits.  The
Linux builder proves relative-to-L0 offsets in its offset mode; Windows's
resource representation does not by itself prove that each raw value is an
independent absolute allocation.  Exact table emission and the unresolved
L8-rooted tail are in
[resource-layout-clear-cache.md](resource-layout-clear-cache.md).

### Exact Windows sampler-format table

The texture-only lookup at `VTGUModeDX32A:0x1020a660` and its byte-identical
OpenGL counterpart at `VTGOGL32A:0x1025d590` are deliberately broader than
the render-target lookup.  Combining it with the D3D public-format converter
at `0x100c7590` gives these complete supported public chains:

| public D3D format | private | type-02 subaddress `30[23:16]` |
|---|---:|---:|
| `A8` | `0806` | `1b` |
| `P8` | `0802` | `15` |
| `L8` | `0805` | `13` |
| `A4L4` | `0813` | `14` |
| `A8L8` | `1029` | `15` |
| `L16` | `1028` | `16` |
| `R5G6B5` | `1018` | `89` |
| `X1R5G5B5` | `101a` | `88` |
| `A1R5G5B5` | `1019` | `8a` |
| `A4R4G4B4` | `101b` | `8b` |
| `A8R8G8B8` | `2030` | `99` |
| `X8R8G8B8` | `2031` | `98` |
| `A8B8G8R8` | `2038` | `b9` |
| `X8B8G8R8` | `2039` | `b8` |
| `A2R10G10B10` | `203a` | `9a` |
| `A2B10G10R10` | `203b` | `ba` |
| `G16R16` | `203c` | `bb` |
| `V8U8` | `1021` | `38` |
| `L6V5U5` | `1022` | `39` |
| `X8L8V8U8` | `2034` | `3a` |
| `Q8W8V8U8` | `2035` | `3b` |
| `V16U16` | `2036` | `3c` |
| `A2W10V10U10` | `2041` | `3d` |
| `D24X8` | `2032` | `fe` |
| `D24S8` | `2033` | `fe` |
| `R16F` | `102c` | `f0` |
| `G16R16F` | `203d` | `f4` |
| `R32F` | `203e` | `f5` |
| `YUY2` | `1023` | `30` |
| `DXT1` | `10400` | `29` |
| `DXT2`, `DXT3` | `20807` | `2a` |
| `DXT4`, `DXT5` | `20809` | `2b` |

The same lookup also has hardware codes for several proprietary private
formats.  Cross-checking the D3D public-format converter proves
`080a -> 0b` is the luma-plane fetch used by planar YV12,
`080e -> 33` is AI44, and `2044 -> 31` is packed AYUV.  YV12 still requires
companion plane state beyond this ordinary one-texture oracle.  AYUV's
conversion state and AI44's exact palette upload now have bounded probes
below, but neither format is live-validated yet.
Private `182d -> b8` is X8B8G8R8, but its public origin remains unresolved:
public 24-bit `D3DFMT_R8G8B8` converts to the distinct and sampler-rejected
code `182f`.  None of these four should be exposed by the initial driver.
Conversely,
the D3D converter recognizes formats which this sampler lookup rejects:
`R8G8B8`, D16/D32/D32F, S8, and the 64/128-bit RGBA float formats are examples.
This is direct evidence that Mesa must advertise sampler, render-target, and
depth/stencil capabilities independently.  In particular, depth sampling is
proved here only for the 24-bit `2032/2033` pair through code `fe`; it is not
safe to infer D16 or D32 shadow-texture support from depth-buffer support.

## Exact sampler state

The sampler block is fully decodable, including the formerly misleading
constant `0x40` in the second word:

| word field | manual meaning | oracle value |
|---|---|---:|
| `31[20:16]` | maximum anisotropy ratio | `16` |
| `31[15:13]` | S enlargement filter | `0`, nearest |
| `31[12:10]` | S shrinking filter | `0`, nearest |
| `31[9:7]` | T enlargement filter | `0`, nearest |
| `31[6:4]` | T shrinking filter | `0`, nearest |
| `31[3:0]` | mip/D shrinking filter | `0`, always level 0 |
| `32[21:19]` | R enlargement filter | `0`, nearest |
| `32[18:16]` | R shrinking filter | `0`, nearest |
| `32[8:6]` | R address mode | `1`, clamp |
| `32[5:3]` | T address mode | `2`, repeat |
| `32[2:0]` | S address mode | `2`, repeat |

Thus `31100000` means nearest/no-mip filtering with an anisotropy *limit* of
16; it does not enable anisotropic filtering.  `32000052` is
`R=clamp,T=repeat,S=repeat`.  In particular, the Linux expression
`32000040 | (filter << 3) | address` places `0x40` in the R-axis field.  It is
not an undocumented enable bit.  The two arguments named `filter=2` and
`address=2` in the recovered Xorg call are the T and S wrap encodings in this
path, despite the historically confusing local names.

The Windows address-mode builder is
`VTGUModeDX32A.dll:0x101d9250`; the OpenGL DLL contains the same routine at
`0x10241a70`.  It maps three private/D3D U,V,W values independently into
`32[2:0]`, `[5:3]`, and `[8:6]`.  The lookup tables at
`0x10335f24` and `0x10399044` are identical:

| D3D address value | API name | hardware code |
|---:|---|---:|
| 1 | WRAP | 2, repeat |
| 2 | MIRROR | 3, mirror |
| 3 | CLAMP | 1, clamp |
| 4 | BORDER | 0, border color |
| 5 | MIRRORONCE | 7, mirror once |
| out of range | clamped fallback | 1, clamp |

The Windows filter builder is `VTGUModeDX32A.dll:0x101d9380`, duplicated at
`VTGOGL32A.dll:0x10241ba0`.  Its private magnification and minification values
produce the following hardware fields in S, T, and (where meaningful) R:

| private value | filter behavior | hardware code |
|---:|---|---:|
| 1 | point/nearest | 0 |
| 2 | linear | 1 |
| 3 | linear anisotropy | 2 |
| 4 | 4x4 kernel, coefficient set A | 3 |
| 5 | 4x4 kernel, coefficient set B | 3 |
| other | nearest fallback | 0 |

Values 4 and 5 load different 32-dword coefficient tables; attaching the
public D3D `PYRAMIDALQUAD` and `GAUSSIANQUAD` names still requires tracing the
earlier API-to-private conversion.  Mip filtering is copied into `31[3:0]`,
with values 3 and above forced to the documented linear value 2.  Maximum
anisotropy is copied into `31[20:16]`.  The same routines are installed in
multiple dispatch-table slots in the OpenGL DLL (for example around
`0x103957c8..0x10395814`), so this is shared GL/D3D hardware-state logic, not
an accidental dead byte sequence.

Two exact guarded successors can validate the ordinary linear and mirror
paths without changing shaders or resource layout.  For linear filtering,
fill columns 0--15 of a 32-square XRGB source red and columns 16--31 green.
Give all vertices constant S/T=`0.5` (`3f000000`), set sampler A31 to
`31102490` (linear S/T minification and magnification, no mip filter), and
retain edge addressing A32=`32000049`.  Every covered pixel must be yellow
with R/G independently in `{0x7f,0x80}` and B zero.  A pure red or green
result proves nearest fetch.  The coordinate is twelve texels from every
edge, so a wrong filter cannot cross either mapped guard.

For wrap modes, instead use four eight-column vertical bands: blue, red,
green, white.  Keep nearest A31=`31100000`, set both S and T to the same
tested mode, and hold T at `0.5`.  Run two constant-S draws, `-0.375`
(`bec00000`) and `+1.375` (`3fb00000`).  Expected XRGB pairs are:

| A32 | mode | result at `-0.375` / `+1.375` |
|---:|---|---|
| `32000049` | clamp-to-edge | blue / white |
| `32000052` | repeat | green / red |
| `3200005b` | mirrored repeat | red / green |
| `320001ff` | mirror-once candidate | red / white |

The mirror-once word includes code 7 in R, T, and S; R is unused but setting
all axes consistently avoids retaining the old clamp field.  For the other
three words R remains code 1.  Both out-of-range S values are only twelve
texels beyond a 32-wide row before address processing.  With the existing
full-page source and adjacent guards even a completely failed wrap unit stays
mapped; preserve and compare both guard pages after each draw.  Do not expose
mirror-once as Gallium mirror-clamp semantics until the final red/white pair
passes, because the Windows table proves only the hardware code, not its API
edge convention.

### Exact border color

The address value `BORDER -> 0` is not sufficient by itself.  The shared
border-color builder is at `VTGUModeDX32A.dll:0x101d9860..0x101d9a2a`; the
OpenGL DLL has the same instruction body at
`VTGOGL32A.dll:0x10242080..0x1024224a`.  For sampler `n` it reads the bound
texture pointer from `context + 0x22bc + n*0x100 + 0xf0` and the public
`D3DSAMP_BORDERCOLOR` ARGB8 value from that record at `+0x44`.  With no bound
texture it deliberately does nothing.

For every texture format except private `1023` (YUY2), helper `0x101d7ab0`
expands the public color to four floats in the order R, G, B, A and the builder
encodes:

| hardware state | bits | value |
|---|---:|---|
| type-02 subaddress `35` | `23:12` | R in sign-magnitude s1.10 |
| type-02 subaddress `35` | `11:0` | G in sign-magnitude s1.10 |
| type-02 subaddress `36` | `23:12` | B in sign-magnitude s1.10 |
| type-02 subaddress `36` | `11:0` | A in sign-magnitude s1.10 |

The exact converter is D3D `0x1020aaa0`, duplicated as OGL `0x1025d9d0`.
Zero maps to `000`; a finite magnitude below one is truncated to ten
fractional bits; a nonzero magnitude at or above one saturates to `400`; and
the input float sign becomes bit 11.  Thus the code is sign-magnitude, not a
12-bit two's-complement integer.  Ordinary ARGB8 components are nonnegative,
so an API component `c` is effectively `(c * 0x400) / 255`, truncated.  The
words reside at `state + 0xed0 + n*0xdc + 0xd4/+0xd8`.

Linux `STM_BorderColor_INV:0x1b1c40..0x1b1d8a` independently performs the
same ARGB unpack, calls `INV_FloatToS_1_10` four times, and dirties both words.
For sampler zero, useful packet-exact examples are:

```text
ARGB 00000000 transparent black: A35=35000000 A36=36000000
ARGB ff000000 opaque black:      A35=35000000 A36=36000400
ARGB ffff0000 opaque red:        A35=35400000 A36=36000400
ARGB ffffffff opaque white:      A35=35400400 A36=36400400
ARGB 80808080 half gray/alpha:   A35=35202202 A36=36202202
```

These are type-02 sampler words, not the type-01 blend words with the same
subaddresses.  They must be emitted in sampler zero's type-02 state group.
The half-gray code is `floor(128*0x400/255)=0x202`.

YUY2 has a distinct path, exactly matching the manual's warning that a YUV
texture needs a YUV border color.  D3D helper `0x101d7b20` (OGL
`0x10240650`) computes, from ARGB8 R/G/B/A values:

```text
Y = ( 0.299 R + 0.587 G + 0.114 B +  16) / 255
U = (-0.169 R - 0.331 G + 0.500 B + 128) / 255
V = ( 0.500 R - 0.419 G - 0.081 B + 128) / 255
A = A / 255
```

The float-to-fixed helper (`0x1020a9f0`, OGL `0x1025d920`) produces a
saturating eight-fractional-bit unsigned value.  The builder masks it to ten
bits, although the conversion itself yields only `0..255`, and packs:

| hardware state | bits | value |
|---|---:|---|
| type-02 subaddress `35` | `23:14` | U8 |
| type-02 subaddress `35` | `11:2` | Y8 |
| type-02 subaddress `36` | `23:14` | V8 |
| type-02 subaddress `36` | `11:2` | A8 |

Bits `13:12` and `1:0` of each YUY2 word remain zero.  After updating either
path, the routine marks both subaddresses dirty (`0x200000` and `0x400000` in
the per-sampler dirty mask) and increments the dirty-range count only for a
newly set bit.  A first Mesa implementation should therefore keep
clamp-to-border capability disabled until it emits `35/36`; falling back to
address code zero with stale or default border words is observably unsafe.

For an eventual guarded live test, source, target, both guard pages, command
ring, and GART/PTE state must remain kernel-owned until idle.  Prefill the
source with an asymmetric pattern or four colored quadrants; a uniform source
cannot distinguish correct sampling from constant output.  Retain the proven
cache flush, two-phase CR prime, backup/restore, bounded waits, and reboot
recovery.  Do not test an incomplete vertex list.

The smallest packet-exact border-color derivative can now reuse the complete
single-texture draw.  Fill a 32x32 BGRX source opaque red, use nearest REPLACE,
and set every vertex S/T to float `-0.25` (`be800000`).  This is far enough
outside to select the border but, if the mode malfunctions into a fetch, the
corresponding linear displacement still fits inside the mapped preceding
4-KiB guard page.  Replace the eight-dword sampler packet with:

```text
FE020440 0000043c 20020000 cc000000
30000000 31100000 32000040 35000400
36000400 cc000000 cc000000 cc000000
```

`32000040` selects border on S/T and clamp on unused R.  A35/A36 encode opaque
green (`ARGB=ff00ff00`).  The covered XRGB destination must therefore be
`0000ff00`; clamp, repeat, mirror, or a stale border color produces a different
result.  The sampler group grows by four aligned dwords, so the exact complete
body lengths become 228 for REPLACE and 232 for MODULATE, and every later
packet index moves by four.  With the blend-aware closed destination group,
the corresponding totals are 232 and 236.  A kernel builder that appends only
A35/A36 but forgets the emitter's 16-byte next-header alignment will assert
the wrong length even though the register words themselves are correct.

Stored border alpha has an extreme-value successor once BGRA A52=`99` and
source-over are live.  Initialize the target to transparent green
`0000ff00`, retain the guarded negative coordinates, and use an otherwise
identical red border twice:

```text
transparent red: A35=35400000 A36=36000000 -> target remains 0000ff00
opaque red:      A35=35400000 A36=36000400 -> target becomes ffff0000
```

Use RGB factors `SRC_ALPHA, INV_SRC_ALPHA` and alpha factors
`ONE, INV_SRC_ALPHA`.  This proves that A36's low 12 bits reach sampled alpha,
not just that A35/A36 RGB and BORDER address mode work.  The same body indices
and 232-dword blend+border REPLACE length apply; A52=`99` is destination state
at body index 6 and must not be confused with any sampler-format byte.

## Measured live result

The kernel-owned test used the oracle's exact local-VRAM addresses,
`source=0x07fc0000` and `target=0x07fd0000`.  It filled the 32x32 source with
opaque red, green, blue, and white quadrants, surrounded the source and target
with full-page guards, and preserved every touched byte for restoration.  The
emitter also rejected the job before engine setup unless its generated body
was exactly 232 dwords.

On the live `1106:7122` the CR reached the aligned PAUSE at `0x570`, engine
status and error were zero, and the result was:

```text
changed=1024
samples=00ff0000/0000ff00/000000ff/00ffffff
bad_pixels=0 bad_source=0 bad_guard=0 restored=yes
```

Thus the recovered texture base, XRGB format, dimensions, tight linear pitch,
nearest sampling, clamp/repeat state, H6 eight-dword FVF/linkage, five-
instruction VS, texture-address instruction, PS ALU instruction, and immediate
texture coordinates form a complete working texture path.  The persistent
test module is
`/root/tmp/chrome9/chrome9-h6-texture-oracle.ko`, SHA-256
`97db1f9b3e425b35ba86f1c7084712b3af295e67b85fdef23461d8ce0be4f543`.

### DMA-GEM source and target

A follow-up used one six-page contiguous DMA-GEM allocation and GART PTEs
1--6.  The target occupied GPU page `0x2000` between PTE1/PTE3 guards; the
texture occupied GPU page `0x5000` between PTE4/PTE6 guards.  Destination A51
used `01=SF`, and texture A18 bits 1:0 used the independently recovered same
`01=SF` selector.  A CPU-side preflight after cache publication first proved
the source and all guards visible before engine setup.

At DMA `0x010c0000`, PTEs `000010c0..000010c5`, the exact 232-dword body
again produced all 1,024 pixels and the four expected quadrant samples.  The
source, four guard pages, local-VRAM 2D regression, CR retirement, and engine
status all passed.  This proves that both texture reads and color-target writes
operate through the translated SF/GART domain used by DMA-GEM.  The module is
`/root/tmp/chrome9/chrome9-h6-sf-texture-fixed.ko`, SHA-256
`cc77856b4887870ad2a775accc061640a455e7a540902855301fbb7ea3f41158`.

### Cached shmem source and target

Windows KMD analysis subsequently identified internal RTSF PTE bit 25 as the
cached/snooped attribute.  Replacing the contiguous DMA-GEM allocation with a
six-page cached GEM-shmem SG mapping and setting that bit in every resource
PTE produced the same complete result at high PFNs.  The live allocation began
at DMA `0xddcca000`; PTEs were `020ddcca..020ddccf`.

The preflight, all 1,024 destination pixels, four quadrant samples, source
contents, four guards, local 2D regression, CR PAUSE, and engine/error status
all passed.  This is the decisive resource result for Mesa: texture reads and
color-target writes work from ordinary cached shmem when the page table
describes its cache policy correctly.
