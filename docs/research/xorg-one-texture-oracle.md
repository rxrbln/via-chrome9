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

The current kernel draw path streams one independent-list triangle rather
than Xorg's four-vertex fan rectangle.  Its silicon-safe two-texture
translation keeps every block and index above through the vertex header at
228 except for the already live topology tuple in the H6 group:

```text
Xorg fan:    A20=201C5C00 A23=23000F02 A24=24000004
kernel list: A20=20000000 A23=23004F02 A24=24000003
```

It then supplies three ten-dword vertices instead of four.  D4 linkage comes
from `0000040A`, `05008442`, and `624081FF`; it does not require the fan's A20
reuse fields.  The exact triangle body is therefore
**262 dwords**, or **266** with the closed four-dword blend destination delta.
Do not drop the pixel ALU's `CCCCCCCC/DDDDDDDD` terminators: they occupy body
224/225 (plus the blend delta), followed by the two alignment words at
226/227, before the vertex H2 header.  The RGBA-swizzle candidate below is
body dword `217 + blend_delta`; a later stage-0 mip table moves it to
`229 + blend_delta`.  These assertions translate the vendor oracle directly
to the existing three-vertex builder without treating the fourth rectangle
vertex as required hardware state.

### State-complete one-texture blend plus D16 map

Closing the destination constants adds four dwords before texture state, and
adding A10--A13/A20 to the common type-01 group adds four more before general
texture state.  For the three-vertex MODULATE path the exact combined body is
therefore **236 dwords**:

| dwords | block |
|---:|---|
| `0..19` | destination, closed A38--A3F, blend equations, A33/pad |
| `20..31` | texture-0 image state |
| `32..39` | texture-0 sampler state |
| `40..55` | common render state plus D16 A10--A13/A20 |
| `56..67` | general one-texture state |
| `68..99` | one-texture H6 FVF/linkage |
| `100..107` | PS constants |
| `108..127` | VS float and integer constants |
| `128..135` | clip A80--A83 |
| `136..159` | five-instruction VS |
| `160..167` | VS activation |
| `168..187` | one-texture PS controls |
| `188..195` | one TAU |
| `196..207` | MODULATE ALUs |
| `208..235` | immediate header plus three eight-dword vertices |

In particular, body 120--127 is not clip state:

```text
FE020440 0000043C 33030000 CC000000
00000000 00000000 00000000 00000000
```

It is the unchanged VS integer-constant H2 packet.  Clip immediately follows
as `FE020440 0000043C 00010000 CC000000` plus A80--A83.  With the kernel's
six-dword leading H0 and two alignment pads, body zero is at slot offset
`0x20`; body 120 is therefore exactly offset `0x200`.  A `043c` readback equal
to slot base plus `0x200` points at this packet in fetch accounting, not at
the clip header and not necessarily at decoder retirement.  The original
one-texture predecessor already put its clip header at the same absolute
offset, so the address alone does not justify padding or changing H2 framing.

### State-complete two-texture plus blend, alpha test, and D16

The two-texture successor can remain compact while closing all state needed by
the current blend and depth paths.  Always expand the destination group to all
eight A38--A3F constant words, even when blending is disabled.  Its final
alignment slot can carry A33 because type-01 words are independently
subaddressed and the proprietary A33 builders establish no ordering dependency
on A00.  The first block is consequently:

```text
body[0..3]    type-01 destination header
body[4..6]    A50, A51, A52
body[7..14]   A38..A3F closed blend constants
body[15..18]  A34..A37 blend equations
body[19]      A33 when alpha test is enabled; cc000000 otherwise
```

This placement writes the comparison before the later common group enables
alpha test.  It avoids growing a 12-word common payload to the next four-dword
alignment boundary solely for A33.

Insert D16 A10--A13 and the established wide-open A20 after A89 in the common
type-01 group.  The old seven-word payload plus those five words is exactly 12
dwords and needs no padding:

```text
body[60..63]  type-01 common-state header
body[64..70]  A86, A87, A53, A00, A01, A88, A89
body[71..75]  A10, A11, A12, A13, A20
```

Here A00 is the render-target blend/alpha-test register and A01 enables D16
test/write.  Do not confuse it with the later subtype-FE general-texture A00:
that word must remain `00000021` for two active stages plus the one-shot
texture-cache clear.  Likewise keep general A08=`08010102`.

The resulting exact single-level, three-vertex map is:

| dwords | block |
|---:|---|
| `0..19` | closed destination/blend state, optional A33 |
| `20..31` | texture image 0 |
| `32..39` | sampler 0 |
| `40..51` | texture image 1 |
| `52..59` | sampler 1 |
| `60..75` | common render/depth state |
| `76..87` | general two-texture state |
| `88..119` | two-texture H6 FVF/linkage |
| `120..127` | PS constants |
| `128..147` | VS constants |
| `148..155` | clip |
| `156..183` | six-instruction VS |
| `184..191` | VS activation |
| `192..211` | two-texture PS controls |
| `212..219` | two TAUs |
| `220..233` | two ALUs and terminators |
| `234..235` | alignment before vertex H2 |
| `236..269` | header plus three ten-dword vertices |

The complete body is therefore **270 dwords**: four for closed blend constants
and four for depth beyond the 262-dword triangle.  A23/A24 are at 102/103, the
RGBA-by-RGBA source-C swizzle is at 225, and the vertex header begins at 236.
Adding only the stage-0 mip table makes 282 dwords and moves those later
indices by 12; mip tables for both stages make 294 dwords.

An exact combined discriminator uses format-99 textures and target, nearest
filtering, clamp, the RGBA-by-RGBA program, canonical source-over, alpha
LESS/ref `80`, and D16 LESS/write.  Clear the target to transparent black and
depth to `ffff`; use uniform texture 0=`80ff8040`, texture 1=`80ffff80`, and
draw at Z=`4000`.  Correct two-texture multiplication produces approximately
`40ff8020` before tests/blending.  It passes alpha test, blends over black, and
stores approximately:

```text
color = 40402008  (allow one LSB per multiplied RGB component)
depth = 4000       (allow the established conversion tolerance)
```

A missing second stage leaves alpha `80` and fails LESS, preserving
zero/`ffff`.  The vendor mask-only `r2.wwww` swizzle instead produces about
`40201008` after blending.  A killed color with depth `4000` exposes alpha
kill after the depth write.  Thus one guarded vector discriminates stage
count, RGBA swizzle, alpha-before-depth ordering, source-over, stored alpha,
and D16 relocation without any out-of-range sample.

Follow it, without reinitializing the engine, by the proven one-texture
REPLACE path drawing opaque blue to a BGRX target.  It must store `000000ff`
and leave the old D16 contents byte-for-byte unchanged.  That transition
requires the one-texture packet to restore subtype-FE A00/A08 to
`00000011/08010101`, upload the one-TAU program, clear type-01 alpha/depth
enables, and restore full-mask format-98 COPY.  Then repeat the combined draw
to prove the two-stage/depth state is restored in the other direction.

The corresponding submit path needs four independently guarded GEM objects,
not reuse of the present `depth` slot for a texture.  A simple contiguous SF
mapping order is target, texture 0, texture 1, depth; each relocation points
one page into its own object:

```text
target = sf_base + PAGE_SIZE
tex0   = sf_base + target_object_size + PAGE_SIZE
tex1   = sf_base + target_object_size + tex0_object_size + PAGE_SIZE
depth  = sf_base + target_object_size + tex0_object_size +
         tex1_object_size + PAGE_SIZE
```

Range-check every addition, concatenate each object's PTEs in exactly that
order, and reject all pairwise object aliases for the first ABI.  Lock all four
reservations together.  The target and depth receive write fences; textures
receive read fences and TO_DEVICE ownership transitions.  Initialize and
verify first/last-page guards for all four objects, and retain every reference,
mapping, PTE array, and SF node through retirement or quarantine.  A timeout
must quarantine the complete four-object mapping just like the current
two-object depth/texture paths; partial early unbind is a GPU-use-after-free
hazard rather than merely a failed draw.

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

### Kernel-builder-ready three-vertex RGBA packet

The current `chrome9_build_textured_triangle()` can grow into this path
without changing its emitter conventions.  The exact meaningful payload
arrays (alignment remains the responsibility of `chrome9_emit_header2`) are:

```text
two_config[5] =
  00000021 01002521 08010102 05000000 0D810000

two_raster_fvf[27] =
  0000040A 010000C2 020041D1 030052D0
  04006342 05008442 1200001F 61800000
  624081FF 20000000 23004F02 24000003
  25F6CC00 28FFCC00 40800000 41400000
  4200493F 43800000 44400000 4500493F
  46800000 47400000 4800003F 49000000
  50000000 51000000 52147FFF

two_vs[24] =
  00380150 00000000 00000000 0008010F
  0008014C 00100150 00384154 0004021F
  0008414C 00000000 00000000 0008422F
  0008814C 00000000 00000000 0008423F
  0008C14C 00000000 00000000 0008028F
  0009014C 00000000 00000000 0008029F

two_vs_control[1] = 60800005

two_ps_control[13] =
  90018000 91000003 92020002 93000002
  94000000 95000000 96000000 97000000
  98000000 99000000 9A000000 CCCCCCCC DDDDDDDD

two_tau[4] = 301001B0 311211B1 CCCCCCCC DDDDDDDD

two_rgba_program[10] =
  0140B6D0 30202053 00F20005 0000007F
  01C00000 301C0000 00F20105 000D083F
  CCCCCCCC DDDDDDDD
```

The corresponding H2 paratypes are exact and match the current emitter call
shape `(transspace=440, transset=0000043c)`: config `fe020000`, raster/FVF
`00040000`, VS `31030000`, VS control `00040000`, PS control `00010000`, TAU
`21030000`, ALU `20030000`, and immediate vertices `00000000`.  Keep the
existing H3 VS-float constants header and all unchanged H2 headers verbatim.

`020041D1` deliberately retains the kernel's live-proven normalized packed
D1 declaration.  The unmodified Xorg program uses `D0` because it ignores
D1; either value is inert for this exact two-sample shader, while `D1` avoids
reintroducing the already diagnosed unnormalized-color trap when a later
program consumes primary color.  The independent-list A20/A23/A24 values are
mandatory for three submitted vertices and are unrelated to D4 linkage.

Each texture image payload has six meaningful words and each sampler has four:

```text
image0: base0>>8 18001101 A20_0 21000001 A22_0 A30_format_0
sampler0: 30000000 A31_0 A32_0 CCCCCCCC
image1: base1>>8 18001101 A20_1 21000001 A22_1 A30_format_1
sampler1: 30000000 A31_1 A32_1 CCCCCCCC
```

Emit them with H2 selectors `00020000`, `20020000`, `01020000`, and
`21020000` respectively.  `A20_n = 20000000 | height_n<<12 | width_n`;
the initial path should retain the existing POT-only A22 calculation and
canonical 32-byte-aligned pitch validation independently for both images.
The format words are `30980000` for XRGB and `30990000` for BGRA.  Sampler
A30 is a different register despite the repeated subaddress.

With the unchanged destination/common/constant/clip blocks, emit calls occur
in exactly this order:

```text
destination, image0, sampler0, image1, sampler1, common, two_config,
two_raster_fvf, PS constants, VS float constants, VS integer constants,
clip, two_vs, two_vs_control, two_ps_control, two_tau, two_rgba_program,
3 * ten-dword vertices
```

The no-blend, no-mip body is exactly 262 dwords:

| dwords | block |
|---:|---|
| `0..15` | destination |
| `16..27` | image 0 |
| `28..35` | sampler 0 |
| `36..47` | image 1 |
| `48..55` | sampler 1 |
| `56..67` | common |
| `68..79` | `two_config` |
| `80..111` | `two_raster_fvf` plus one emitter pad |
| `112..147` | PS/VS constants and clip |
| `148..175` | `two_vs` |
| `176..183` | VS control |
| `184..203` | PS control plus emitter padding |
| `204..211` | two TAUs and terminators |
| `212..225` | two ALUs and terminators |
| `226..227` | emitter padding before vertices |
| `228..261` | vertex header plus `3 * 10` dwords |

In that body `base0>>8` is index 20, `base1>>8` is index 40, the RGBA
swizzle is index 217, and the immediate header is index 228.  Closed blend
state adds four dwords after the destination format, so add four to every
index from image 0 onward and expect 266 total; target A50 remains index 4.

Useful pre-publication assertions for the no-blend/no-mip body are:

```text
[20]=base0>>8   [32]=sampler0 A30   [40]=base1>>8   [52]=sampler1 A30
[72]=00000021   [74]=08010102
[84]=0000040A   [89]=05008442       [92]=624081FF
[93]=20000000   [94]=23004F02       [95]=24000003
[172]=0009014C  [175]=0008029F      [180]=60800005
[190]=92020002  [208]=301001B0      [209]=311211B1
[216]=0140B6D0  [217]=30202053       [219]=0000007F
[220]=01C00000  [223]=000D083F      [224]=CCCCCCCC [225]=DDDDDDDD
[228]=FE020440  [230]=00000000
```

For blend add four to every listed index except destination/target words; for
the stage-0 mip form add another twelve to assertions at sampler 0 and later.
The mip header itself is `[28+B]=FE03043C`, `[29+B]=8`, `[30+B]=10030000`,
with entries at `[32+B..39+B]`.

For the stage-0 mip successor, insert exactly one call
`chrome9_emit_header3(..., 10030000, mip0, 8)` between image 0 and sampler 0.
For every used L1--L8 slot, `mip0[l-1]` is
`40000000 | ((level_offset[l] >> 8) & 00ffffff)`; clear every unused slot.
Set sampler-0 A30 max/min/bias explicitly and keep image-0 A18=`18001101`.
The exact no-blend map then has image 0 at `16..27`, mip table `28..39`,
sampler 0 `40..47`, image 1 `48..59`, sampler 1 `60..67`, and all later
blocks shifted by 12.  Total length is 274, base1 is index 52, RGBA swizzle
is 229, and the vertex header is 240.  Blend gives 278 and adds four to those
post-destination indices.

The current submission path treats its second object (`depth_obj`) as the one
texture.  A second texture therefore requires a real third GEM object in the
job rather than a second address into an unreserved BO: lookup and validate
all three, reserve all three in `drm_exec`, collect all three SG tables into
the contiguous PTE list, include all sizes in the `drm_mm` node, map/guard/
flush/sync all three, attach the completion fence to all three reservations,
and mirror all three through normal cleanup and quarantine ownership.  With
the existing `[guard,data,guard]` object convention:

```text
target = sf_start + PAGE_SIZE
base0  = sf_start + destination_object_size + PAGE_SIZE
base1  = sf_start + destination_object_size + texture0_object_size + PAGE_SIZE
```

Initially requiring three distinct objects keeps reservation and guard
ownership unambiguous.  Sum sizes with checked overflow and retain the current
GART/ring bound.

#### Three-BO lifetime and error-unwind audit

The present shared submit routine names the one texture `depth_obj` and stores
it in `job->depth`.  A safe successor may rename those fields or add explicit
texture names, but it must extend every phase below; changing only the builder
address creates an unreserved, unpinned fetch:

| phase | exact three-object obligation |
|---|---|
| request validation | compute destination, texture-0, and texture-1 guarded object sizes independently; use checked additions for the three-size total and enforce the existing GART/ring limit |
| lookup | hold three `drm_gem_object_lookup()` references; reject NULL, foreign-device, imported, wrong-size, and any pairwise alias for the first ABI |
| job construction | add texture-1 shmem object, SG table, map, mapped flag, and page count; include all three counts in `job->page_count` |
| PTE collection | allocate one total PTE array; append destination, texture 0, then texture 1 at offsets 0, `destination_page_count`, and `destination_page_count + texture0_page_count` |
| reservation | initialize `drm_exec` for three objects and prepare all three inside the retry loop; because guard initialization writes every BO, use the write-access dependency class (`dma_resv_usage_rw(true)`) so both prior readers and writers finish |
| CPU maps | vmap destination, texture 0, then texture 1; reject `is_iomem` for each and unwind only maps which succeeded |
| SF binding | allocate one node of the checked three-object total, install the complete PTE array, and derive the three data addresses using full guarded object sizes |
| device handoff | prepare both guards, cache-flush the full object, and call `dma_sync_sgtable_for_device(..., DMA_BIDIRECTIONAL)` for each object |
| fences | attach the same initialized job fence with WRITE usage to the target and READ usage to both texture sources before publishing the ring |
| retirement | sync all three SG tables for CPU, flush all three CPU maps, execute `dma_rmb()`, then validate both guards for each object before unbinding PTEs |
| normal unwind | if device-owned, reverse DMA ownership/cache state for all three; vunmap texture 1, texture 0, destination in reverse-success order; finish `drm_exec`, signal when required, put all three GEM references, then drop the fence |
| pre-fence failures | before `dma_fence_init64`, free the total PTE/job allocation and put every successfully looked-up object; do not enter the fence-release path |
| quarantine | on timeout or failed PTE invalidation, finish `drm_exec` and signal the error fence but deliberately retain **all three** maps, GEM references, PTEs, SF node, and DMA ownership in the quarantined job; never unwind only texture 1 while hardware may still fetch it |

In particular, a failure of texture-1 lookup must still put destination and
texture 0; a failure of its SG-table acquisition or PTE collection must free
the total PTE/job state and put all three lookup references; a failure of its
vmap must unmap the two earlier successful maps.  After fence initialization,
all errors converge through the existing `out_exec`/fence lifetime rather
than directly freeing the job.  Extend both successful and `device_owned`
cache/DMA reversal blocks symmetrically—missing the latter leaves a partial
builder failure with texture 1 still device-owned.

### Recommended next gate: D16 lightmaps, then one-state batching

After the three-object RGBA oracle passes, ioquake3's smallest useful
successor is two-texture RGBA MODULATE with the already proven linear D16
LESS/write state.  Do this before blend, alpha test, mipmaps, overbright
scale, or batching so a failure has only one new state group and one new BO.
The base packet already carries normalized Z in D0; no FVF, VS, PS, TAU,
ALU, or vertex-stride word changes.

Replace the common group's trailing `cc000000` pad with the five established
depth words.  The payload grows from eight to twelve dwords, hence the whole
group grows by only four:

```text
body[56..59] common type-01 header
body[60]     860000ff
body[61]     870000ff
body[62]     53010c0f
body[63]     00000011
body[64]     01c0154c                         # A01 D16 LESS/write
body[65]     88001ed0
body[66]     89000800
body[67]     10000000 | (depth_base >> 8)      # A10
body[68]     11000001 | ((depth_pitch/32)<<13) # A11 linear pitch
body[69]     12380000                          # A12 D16
body[70]     13010000                          # A13
body[71]     20007fff                          # A20 wide Z clip
```

The depth-only, no-blend, no-mip three-vertex body is exactly **266 dwords**:

| dwords | block |
|---:|---|
| `0..55` | unchanged destination and two image/sampler pairs |
| `56..71` | common render state plus D16 |
| `72..83` | two-texture configuration |
| `84..115` | two-texture FVF/linkage and list tuple |
| `116..151` | constants and clip |
| `152..179` | six-instruction VS |
| `180..187` | VS activation |
| `188..207` | PS controls |
| `208..215` | two TAUs |
| `216..229` | RGBA ALUs and terminators |
| `230..231` | vertex-header alignment |
| `232..265` | Type-00 header plus three ten-dword vertices |

Assert A20/A23/A24 at `97/98/99`, the RGBA source-C word
`body[221]=30202053`, and the vertex header at 232.  A following non-depth
two-texture draw must restore A01=`01c01540`; stale A10--A13 addresses are
then inert.  Alternate depth and non-depth draws without engine
reinitialization before exposing the capability.

The first D16 ABI should add only a depth handle and canonical D16 pitch to
the existing fixed three-vertex request.  Keep XRGB destination, two POT
level-zero sources, fixed RGBA MODULATE, finite `[0,1]` D3/D4, and three
vertices.  Require four distinct native guarded objects and map them in the
order target, texture 0, texture 1, depth.  Reserve all four together and
concatenate all four PTE sets before binding.  Target and depth get WRITE
fences; both sources get READ fences.  Since guard initialization writes all
four objects, dependency waits must cover prior readers as well as writers.
Every normal retirement, prepublication unwind, PTE-unbind failure, and
timeout quarantine must treat the four maps, SG tables, references, guards,
DMA ownership transitions, and one SF node as an indivisible job.

A safe ordered D16 oracle uses constant in-range coordinates.  Clear XRGB
target to `005a5a5a` and D16 to `ffff`.  First draw the covered half at
Z=`4000`, sampling stage-0 red and stage-1 red; an interior must become
`00ff0000` and depth approximately `4000`.  Submit the same geometry at
Z=`c000` while sampling the yellow product.  LESS must preserve both the red
color and `4000` depth.  The uncovered half remains sentinel/`ffff`; both
source images remain byte-identical; all eight guard pages survive.  Follow
with the base non-depth RGBA ioctl and repeat the D16 sequence to test closure
in both directions.

Only after that passes should one-state immediate batching be added.  Set one
list A24 and stream a continuous `3*N*10`-dword payload; do not concatenate
nominal single-triangle bodies.  For D16 without blend or mips:

```text
A20 body[97] = 20000000
A23 body[98] = 23004f02
A24 body[99] = 24000000 | (3*N)
vertex H2     body[232]
payload       body[236 .. 235 + 30*N]
body length = 236 + 30*N dwords
```

The existing universal two-texture cap of **18 triangles / 54 vertices**
remains safe: N=18 gives a 776-dword body and the normal PAUSE=`0xcf0`,
STOP=`0xd70`; N=19 exceeds the page/read-ahead bound.  Preflight the complete
body and trailer before writing staging memory, copy at most 54 userspace
vertices once with `memdup_array_user()`, validate every triple and all four
S/T values, and emit converted vertices into bounded allocated storage rather
than a multi-kilobyte kernel-stack array.  All destination, source, depth,
sampler, scissor, shader, and depth state is shared across the batch.

The first D16 batch oracle uses two triangles `TL,TR,BR` and `TL,BR,BL` at
Z=`4000` with the RGBW-times-red/red/blue/yellow sources.  Four quadrant
interiors must be red, black, blue, and yellow, and D16 must be approximately
`4000` at each; an outer sentinel and `ffff` depth remain untouched.  Then
submit a deeper red full-quad batch and require all first-pass color/depth
samples to remain unchanged.  This simultaneously proves A24 automatic fire,
ten-dword stride, D4 linkage, depth interpolation, and four-BO lifetime.

Mesa should keep this batch local to one vbuf callback and flush at 18 or on
return, just like the untextured batch.  Enable it only for the exact
`T0|T1` classifier, two supported sampler/view slots, Z16 LESS with writes,
no blend/alpha test, and the fixed two-texture capability pair.  Preserve
primitive expansion order and per-triangle nondegenerate checks.  On one
failed ioctl reject the entire chunk and stop that callback; never retry its
triangles individually against a possibly wedged engine.

#### Exact first raw RGBA oracle

Use three distinct guarded BOs.  The 64x64 XRGB target has pitch 256 and
`0x4000` useful bytes; initialize every useful dword to sentinel `005A5A5A`.
Each 32x32 XRGB texture has pitch 128 and `0x1000` useful bytes.  Fill texture
0's TL/TR/BL/BR quadrants red/green/blue/white and texture 1's quadrants
red/red/blue/yellow:

```text
texture 0: 00FF0000 0000FF00 000000FF 00FFFFFF
texture 1: 00FF0000 00FF0000 000000FF 00FFFF00
expected : 00FF0000 00000000 000000FF 00FFFF00
```

With 4-KiB guards the exact object sizes are `0x6000,0x3000,0x3000`, total
`0xc000`.  If this job's SF node begins at `S`, assert target=`S+1000`,
base0=`S+7000`, and base1=`S+a000`; their shifted values occupy body dwords
4, 20, and 40 respectively in the no-blend/no-mip packet.

Submit the already-live top-right independent-list triangle with the same
coordinates on both stages:

```text
v0: x/y=-0.5/-0.5, z=0, rhw=1, D1/D2=FFFFFFFF, s0/t0=0/0, s1/t1=0/0
v1: x/y=63.5/-0.5, z=0, rhw=1, D1/D2=FFFFFFFF, s0/t0=1/0, s1/t1=1/0
v2: x/y=63.5/63.5, z=0, rhw=1, D1/D2=FFFFFFFF, s0/t0=1/1, s1/t1=1/1
```

Use nearest filtering and clamp-to-edge independently for both stages.  The
following target samples are far from texture quadrant and triangle edges:

| target pixel | region | exact expected dword |
|---:|---|---:|
| `(8,4)` | covered TL: red times red | `00FF0000` |
| `(48,16)` | covered TR: green times red | `00000000` |
| `(56,48)` | covered BR: white times yellow | `00FFFF00` |
| `(8,48)` | outside the triangle | sentinel `005A5A5A` |

The nonzero sentinel is essential: a zero-initialized target cannot
distinguish the expected black TR sample from a missing draw.  Also require a
covered-pixel count near one half of the 4096-pixel target, exact preservation
of both `0x1000` source ranges, and all six guard pages unchanged.  For the
lowest-risk two-step execution, first use the vendor alpha-mask swizzle
`302026DB` and make texture 1 BGRA with magenta RGB poison and quadrant alpha
`00/ff/80/40` (dwords `00FF00FF/FFFF00FF/80FF00FF/40FF00FF`).  The three
covered samples above must then be black, green, and gray `00404040` while
the outside remains sentinel.  After that retires, refill texture 1 with the
XRGB RGBA vector, change only body dword 217 to `30202053`, and require the
RGBA results above.

The exact stage-0-mip raw successor keeps this geometry and target.  Texture
0 contains 32x32 black L0 (`0x1000` bytes) followed by 16x16 white L1
(`0x400` bytes), so its useful range is `0x1400`, page-rounded data range is
`0x2000`, and guarded object size is `0x4000`.  Texture 1 remains the XRGB
red/red/blue/yellow `0x3000` object.  The three-object total becomes `0xd000`;
for SF node start S, base0 remains `S+7000` while base1 becomes `S+b000` at
body dword 52.  Emit H3 entry 0=`40000010`, clear entries 1--7, and set
sampler-0 A30=`30020040` to force L1.  Exact samples `(8,4)/(48,16)/(56,48)`
must then be red/red/yellow, with `(8,48)` still sentinel.  All-black covered
samples isolate a missing/ignored L1 table or LOD word; corrupt but nonblack
colors isolate the second fetch or RGBA shader.  Verify texture 0's complete
`0x2000` data allocation (including padding), texture 1, and all guards.

For the first public two-texture ABI, validate all four S/T words as finite
and keep them in `[0,1]`.  A merely finite but enormous coordinate is not
made safe by one guard page if a sampler/address-mode regression bypasses
wrap.  Run the small `-0.375/+1.375` wrap discriminator only through a
dedicated guarded validation path with 32-square sources; loosen public
coordinate bounds only after the addressing modes retire safely.  Apply the
same rule independently to D3 and D4.

Finally, state closure must be explicit in both directions.  Every two-stage
draw emits both image/sampler groups, A00=`21`, A08=`08010102`, both TAUs,
PS count `92020002`, six-instruction VS, and ten-dword vertices.  Every later
one-stage draw must continue emitting A00=`11`, A08=`08010101`, one-stage
controls/TAU/VS/FVF, and sampler A30=`30000000`; that disables stale stage 1
and stale mip addresses without depending on old register contents.  Retain
the dual-stage `CC/DD` words, pre/post texture-cache transition, guarded
readback, timeout quarantine, and assertion of the exact body length and
relocation/index words before publication.

Exercise closure as an ordered sequence, not only isolated draws.  First
submit two-stage red-times-black (black expected), then a one-stage green
REPLACE into a disjoint target region (green expected); this catches a stale
stage count, PS, D4 stride, or six-instruction VS.  Next submit forced-L1
green from a red/green mip resource, immediately followed by single-level red
with sampler A30 zero (red expected); this catches retained LOD selection.
Keep both old source objects reserved through each draw's retirement even
when the following draw disables their stage, and verify every source and
target guard after the sequence.

Two follow-ups distinguish per-stage coordinates and sampler state after the
basic same-coordinate nearest test passes.  For the wrap test, fill both
32-square textures with vertical blue/red/green/white eight-column bands.
Use nearest filtering, stage-0 repeat A32=`32000052`, and stage-1 clamp A32=
`32000049`.  Give every vertex D3 S/T=`-0.375,0.5`
(`bec00000,3f000000`) and D4 S/T=`+1.375,0.5`
(`3fb00000,3f000000`).  Stage 0 must repeat to green, stage 1 must clamp to
white, and RGBA multiplication must return green.  Reusing D3 for D4, aliasing
both sampler selectors, or exchanging the two sampler groups produces black
or another primary.  Both hypothetical failed linear fetches are only twelve
texels outside the logical row and remain inside the adjacent mapped guards.

For the filter test, make stage 0's left/right halves red/green and stage 1's
left/right halves blue/white.  Set both coordinates to exactly `0.5`, both
samplers to A31=`31102490` and clamp A32=`32000049`.  Each source is averaged
at the center boundary; their product must have R/G in `0x3f..0x41` and B
zero (nominal XRGB `00404000`).  A nearest or wrong-stage filter yields black,
half-yellow, or a much larger single channel, leaving wide rounding margins.
These tests alter only D3/D4 and the two sampler payloads; packet counts and
all relocation indices remain those of the 262-dword body.

DXT1 can then enter the same two-stage path without a new shader or packet.
Use the documented constant-color DXT1 quadrant blocks for stage 0, set its
image-format word to `30290000`, and retain the 32x32 logical image state;
its compressed useful range is `0x200` bytes.  Use an ordinary XRGB stage 1
with red/red/blue/yellow quadrants.  Nearest RGBA modulation must return
red/black/blue/yellow.  In the no-mip body only image-0 dword 25 changes from
`30980000` to `30290000`; image-1 format remains body 45 (or `45+B+12*M` for
blend delta B and a preceding stage-0 mip table M).  This combined test proves
that compressed block-row addressing and the second fetch coexist, while a
decoder error remains isolated by the already-passing uncompressed version.

R5G6B5 is the lower-risk game-format predecessor to DXT1.  For a 32-square
stage 0 use format word `30890000`, canonical pitch 64, useful range `0x800`,
and quadrant words `f800/07e0/001f/ffff`.  Page rounding still gives the same
`0x3000` guarded object size as the 32-bit raw oracle, so every relocation and
body index remains identical; only image-0 body dword `25+B`, derived cpp,
pitch, and source contents change.  Against the same XRGB red/red/blue/yellow
stage 1, expected RGBA modulation is again red/black/blue/yellow.  Validate
this before compressed formats: it exercises non-four-byte pitch/range logic
without block addressing or alpha ambiguity.

### Ordered ioquake3 follow-on gates: wrap, alpha test, then fog

After two-texture+D16, the first game-visible limitation is the coordinate
validator, not another fragment program.  Tiled base images routinely need
coordinates outside `[0,1]`, while atlas/lightmap coordinates ordinarily stay
inside it.  Alpha-tested lightmapped surfaces are the next bounded state
combination.  Fixed fog is third: its register map is known, but the W/Z
choice and LINEAR/EXP/EXP2 numeric conventions have not retired on silicon.
Do not advertise fixed fog merely because its enable words can be emitted.

#### Gate 1: independent wrapping with out-of-range D3/D4

The existing wrap discriminator above is already the exact first test.  It
changes no packet count, relocation, shader, FVF, or BO lifetime:

```text
stage 0 A32 = 32000052              # repeat
stage 1 A32 = 32000049              # clamp to edge
D3 at all vertices = bec00000 3f000000   # (-0.375, 0.5)
D4 at all vertices = 3fb00000 3f000000   # (+1.375, 0.5)
```

With the blue/red/green/white vertical bands in both 32x32 sources, stage 0
repeats to green and stage 1 clamps to white, so the final RGBA product is
exactly `0000ff00`.  The deliberately small excursions are only twelve texels
past an edge if wrap is accidentally bypassed, keeping a failed linear access
inside the adjacent mapped guard.  Check both complete source data ranges and
all six guards after retirement.  Run the converse state as a second job
(stage 0 clamp at `+1.375`, stage 1 repeat at `-0.375`) and require green
again; this prevents one working sampler from hiding an aliased selector.

The kernel interface must expose normalized per-stage modes and lower them,
not accept raw A32 words.  The exact established encodings are:

| API mode | A32 |
|---|---:|
| clamp to edge | `32000049` |
| repeat | `32000052` |
| mirrored repeat | `3200005b` |
| border | `32000040` |

Keep the first public loosening finite and bounded to `[-0.5,1.5]` independently
for S/T and D3/D4.  This admits the guarded repeat/clamp discriminator without
immediately accepting large arbitrary values.  The one-page failure
containment above additionally depends on that oracle's fixed 32x32 images;
the coordinate bound alone is not a memory-safety substitute for proving the
sampler on larger images.  Mesa must fall back for a draw outside the bound;
clamping the submitted coordinate would silently change REPEAT semantics.
Expand it only after guarded size/coordinate sweeps retire.  MIRROR and BORDER
need their own capability bits and exact oracles and must not become implicit
side effects of proving REPEAT.  In particular, BORDER additionally depends
on the separately documented A35/A36 border-color words.

Follow the wrap draw immediately with the ordinary in-range clamp draw and
then repeat the wrap draw.  Each packet still has the base 262-dword body
(`266` with D16), A20/A23/A24 and the vertex header stay at their existing
indices, and the later clamp packet must write both A32 words explicitly.
There is no new reservation or fence rule: retain both source BOs and the
target through the one job exactly as for the base two-texture ioctl.

#### Gate 2: lightmap product alpha test, then framebuffer blend

Alpha test consumes the RGBA MODULATE program's resulting alpha.  For the
base 262-dword two-texture packet, the common type-01 block has one alignment
slot, so the first no-depth test is size preserving:

```text
body[63]  00100011                  # A00 bit 20, alpha test on RT0
body[67]  33001202                  # A33 LESS, reference 0x80
```

All following indices remain unchanged: A20/A23/A24 are `93/94/95`, the
RGBA source-C word is body 217, and the vertex header is body 228.  Use an
opaque-white stage 0 and submit uniform stage-1 BGRA `40ff0000`, then (only
after retirement) uniform `c000ff00` over the same pixels.  Correct LESS
leaves the first red result.  Disabled alpha test leaves green, reversed
comparison also leaves green, both killed leaves the sentinel, and a missing
second stage leaves the opaque stage-0 alpha and kills both draws.  Both
texture ranges and all guards must remain intact after each submit.

The first alpha-plus-D16 gate should use the already-defined state-complete
270-dword map rather than invent another placement.  Close the destination
group with A38--A3F and the canonical COPY words
`34000150 35000114 36000590 37000224`, and A33 at body 19.  Put D16 state in
common body 71..75 and enable alpha through A00 at body 67.  The important
fixed indices are therefore:

```text
body[19]      33001202              # A33 LESS/ref 0x80
body[67]      00100011              # A00 alpha enable
body[71..75]  A10, A11, A12, A13, A20
body[102..103] A23, A24
body[225]     30202053              # RGBA-by-RGBA
body[236]     immediate vertex header
```

Clear color to zero and D16 to `ffff`.  With opaque-white stage 0, first draw
stage-1 `c0ff0000` at Z=`4000` (killed), then stage-1 `4000ff00` at Z=`8000`
(passes).  The exact covered result is XRGB green with depth approximately
`8000`.  Red/`4000` diagnoses disabled or reversed alpha; zero/`4000`
diagnoses a killed color whose depth write escaped.  This proves that the
dual-sampler product alpha is tested before the depth update.

Opaque lightmapping itself does not require framebuffer blending: the pixel
program has already multiplied base and lightmap.  Add source-over only for
transparent dual-texture surfaces, after the alpha+D16 kill is live.  Use the
same 270-dword state-complete packet, format-99 target, and the established
independent-alpha source-over words:

```text
A34..A37 = 34000020 35000148 36000590 37000244
A00      = 00100011                 # retain RT0 blend; add alpha-test bit 20
```

The existing combined vector—textures `80ff8040` and `80ffff80`, LESS/ref
`80`, Z=`4000`, transparent-black target—must store approximately
`40402008` and depth `4000` (one RGB LSB tolerance).  Keep blend enable,
compare function/reference, and destination format as normalized UAPI fields;
userspace never supplies A00/A33/A34--A37 words.  This extension adds no BO:
the four target/texture0/texture1/depth reservations, mappings, fences,
guards, and timeout quarantine remain one indivisible job.  If alpha or blend
validation fails before publication, unwind the same four-object job; after
publication, quarantine all four on timeout.

Mesa may expose fixed alpha test only when the exact fragment classifier
guarantees a defined output alpha and the corresponding kernel capability is
present.  Otherwise the state tracker can lower alpha test to a discard in
NIR, which this fixed shader path does not implement and must reject.  Test
closure in both directions with an alpha-disabled D16 draw and a one-texture
COPY draw before enabling the path for applications.

#### Gate 3: fixed-fog endpoint experiment only

The lowest-risk fog test avoids every unproved table/range conversion and
uses secondary-color alpha as an explicit interpolated factor.  Append A70--
A72 to the base common group and let the next header align naturally:

```text
body[56..59] common type-01 header
body[60..66] existing A86,A87,A53,A00,A01,A88,A89
body[63]     00000f11               # all proprietary RT fog enables
body[67]     70000020               # secondary alpha, local mode
body[68]     71400000               # fog color red: R=0x400, G=0
body[69]     72000000               # B=0
body[70..71] cc000000               # alignment
```

This moves config to body 72 and makes the packet 266 dwords.  Every later
index shifts by four: the FVF A03 word is body 91 and must change from
`030052d0` to normalized COLOR1 `030052d1`; A20/A23/A24 become `97/98/99`,
the RGBA source-C word becomes 221, and the vertex header becomes 232.  The
ten-dword vertex stride is unchanged and its packed D2 word supplies the fog
factor.  With D16 but no alpha/blend, append A10--A20 before A70--A72; the
common block then ends at 75 and total body length is 270.

Use solid white times solid blue so the PS result is blue, red fog, in-range
coordinates, no blend, and no depth.  Submit two disjoint guarded triangles,
one with D2=`00ffffff` and one with D2=`ffffffff`.  Under the conventional
fog equation factor zero yields exact red and factor one exact blue; exchanged
results identify the opposite convention without being a failure.  First run
a red-product/red-fog no-visible-change control, and follow every fog submit
with a normal two-texture COPY/MODULATE draw that restores A00=`00000011` and
A03=`030052d0` and must produce its unfogged color.

Passing this endpoint proves only A00/A70 enable and source selection, packed
D2 normalization, A71/A72 channel packing, and fog composition after the
known pixel shader.  It does **not** prove eye-Z versus W, A73/A75/A76/A77
linear-range encoding, A78 density, EXP, or EXP2.  Keep it behind an internal
diagnostic cap and do not expose generic Gallium/GL fog until those cases have
guarded numeric sweeps and explicit state-closure tests.

### Bounded immediate triangle-list batching

Invariant H6 state can be shared by consecutive triangles.  The Windows
normal draw callback gathers and serializes only dirty state and then clears
the emitted dirty records (`VTGUModeDX32A.dll:10156c97` calls the gatherer;
`1015741a` calls the clearer).  Consequently an unchanged register is resident
across later draws rather than implicitly reset at PLEND.  This is static
vendor evidence for state persistence, not permission to omit state when a
fresh kernel-owned context/ring is installed.

The safest first batch does not emit several nominal H6 bodies.  It emits the
complete known state once, changes A24 to the total vertex count, and follows
one Type-00 header with one continuous immediate payload:

```text
A20 = 20000000                    # independent triangle list
A23 = 23004F02                    # Gouraud
A24 = 24000000 | (3 * triangles)
payload dwords = 3 * triangles * vertex_stride
```

The first live untextured two-triangle attempt exposed an important template
inheritance bug before validating this list form.  `chrome9_emit_3d_quad_h6()`
contains the four-vertex fan word `201c5c00` at body index 48.  The original
single-triangle derivative changed only A23/A24 at 49/50, so three submitted
vertices could not reveal that A20 was still a fan.  The initial batch did
the same.  Its red first triangle and nominally all-green second triangle
produced `0028d700` at `(16,48)`: for the inset 7.5--55.5 quad, a fan anchored
at the first red TL vertex has red weight `(55.5-48)/48 = 0.15625`, exactly
`40/255`, and green weight `215/255`.  This is precise fan interpolation, not
an attribute-latch anomaly.

Therefore the untextured color builder must explicitly replace
`body[48+B]` with `20000000`, where `B=4` for the closed blend insertion.
The D16 insertion adds four more words before raster state, so its index is
`body[52+B]`; its A23/A24 remain at `53+B`/`54+B`.  Assert A20 together with
A23/A24.  The safest corrected live test is the identical N=2 payload with
only this one-word repair.  Until it passes, regard the batching capacity and
packet maps below as exact construction bounds backed by static list evidence,
not as live validation of Chrome9 immediate-list restart behavior.

The front end fires after every complete three-vertex list primitive and
generates PLEND only after the A24 total is exhausted.  There is no separator,
fire word, or PLEND word between triangles.  For one texture the stride is
eight dwords and for two textures it is ten, so every three-vertex triangle
adds respectively 24 or 30 payload dwords.  The Type-00 header remains four
dwords and emitter padding, if any, follows the complete A24-counted payload.
Type-00 has no second payload-length field: A24 is the only delimiter.  It
must equal exactly `3*N`, and exactly `3*N*stride` dwords must immediately
follow the header.  A count that is too large can consume the post-draw sync
as vertex attributes; a count that is too small can expose leftover attribute
words to the command decoder.  Both failures are wedge-class, so assert the
computed payload end as well as the final body/trailer size.

Do not merely concatenate another Type-00 packet after an A24=`3` payload.
The first list has already exhausted A24.  A genuinely separate primitive
list must program a new A24 before its next Type-00 payload; A20/A23 and the
other unchanged registers persist.  This delimiter is exact vendor behavior,
not only an inference from the self-addressed type-04 format.  Windows helper
`VTGUModeDX32A.dll:101b0a20` emits an aligned type-04 packet whose sole
payload is `24000000 | count`, then aligns and emits the following indexed
payload header.  Helper `101b0ad0` emits only A23 and A24 before its payload;
its callers at `101b0f75` and `101b10dd` use it repeatedly for three-index
lists without replaying invariant render state.  The older Linux H6 GL path
independently does the same in
`__glS3InvDrawPrimitiveTriangleList_H6:215448..2155f2`: it chunks a triangle
list to available DMA space, emits one A24 word for each chunk, emits an H3
inline sequential-index payload, releases the space, and loops to the next
chunk without state replay.  Its capacity calculation subtracts `0x600`
bytes from the command manager's reported space before choosing a chunk.
These are exact static indexed-draw examples rather than a live immediate-
Type-00 re-arm on VX900.  One enlarged A24 immediate list still avoids this
extra transition and is both smaller and safer.  State changes between
triangles should split the first batch/job; a later guarded oracle can
exercise the vendor A24-only delimiter with consecutive immediate payloads.

For the current one-texture builder, let `B=4` when the closed blend
destination block is present and zero otherwise, and let `M=12` when one
eight-entry H3 mip table precedes the sampler and zero otherwise.  With `N`
triangles the exact body maps are:

```text
REPLACE:  A24 body[74+B+M], vertex H2 body[196+B+M],
          payload body[200+B+M ... 199+B+M+24*N],
          body length = 200+B+M+24*N dwords
MODULATE: A24 body[74+B+M], vertex H2 body[200+B+M],
          payload body[204+B+M ... 203+B+M+24*N],
          body length = 204+B+M+24*N dwords
```

The largest count valid for all those one-texture variants is **24
triangles / 72 vertices**, hence A24=`24000048`.  This cap includes blend and
the first mip table.  In the largest MODULATE+blend+mip case the body is 796
dwords, begins at slot offset `0x20`, and ends at `0xc90`.  The unchanged
post-draw sync/status/dummy/cookie sequence ends at `0xcf8`; the current
strict padding then places PAUSE at `0xcf0` and STOP at `0xd70`.
This 24-triangle result is specific to a total post-base delta of at most 16
dwords.  The exact border-color group adds four more dwords: blend+mip+border
therefore has a 20-dword delta and must cap at 23 triangles.  Palette and
de-gamma tables can reduce it further.  Keep 24 as the UAPI maximum, but make
the builder's accepted count depend on the exact generated body length rather
than assuming every future state combination fits that maximum.

The same arithmetic gives the immediately useful untextured Mesa batch.  Let
`D=4*blend_variant + 4*D16_variant`; this is a packet-length delta, not a
hardware bit mask.  The shared map is:

```text
A24 body[50+D] = 24000000 | (3*N)
vertex H2 body[160+D]
payload body[164+D ... 163+D+18*N]
raw body end = 164+D+18*N dwords; align before the post-draw H0
```

A single cap valid for color, blend, D16, and D16+blend is **34 triangles /
102 vertices**, A24=`24000066`.  In the largest D16+blend case (`D=8`) its
raw body is 784 dwords and ends at slot `0xc60`; cookie/PAUSE/STOP are
`0xcc8/0xcf0/0xd70`.  The plain color path alone can fit 35 triangles
(A24=`24000069`, cookie=`0xcf8`), but using 34 uniformly avoids a state-
dependent userspace split threshold.  Its first two-triangle oracle should
use an all-red `TL,TR,BR` followed by an all-green `TL,BR,BL`, A24=`24000006`,
and a non-red/non-green sentinel target.  Interior top-right and bottom-left
samples then independently prove the first and second automatic fires without
depending on interpolation along the shared diagonal.

For the exact two-texture program above, the corresponding map is:

```text
A24 body[95+B+M] = 24000000 | (3*N)
vertex H2 body[228+B+M]
payload body[232+B+M ... 231+B+M+30*N]
body length = 232+B+M+30*N dwords
```

Here `M=12` is the stage-0 mip table and `B=4` is the same blend delta.  The
safe cap is **18 triangles / 54 vertices**, A24=`24000036`.  With both deltas
the 788-dword body ends at slot offset `0xc70`, the cookie ends at `0xcd8`,
PAUSE is `0xcf0`, and STOP is `0xd70`.  N=19 is invalid even without either
delta: its body ends at `0xca8`, the following H0 alignment makes the cookie
end `0xd18`, and `chrome9_finish_ring()` would put STOP at `0xdf0`, beyond the
allowed `0xde0` read-ahead boundary.  The two-TAU/two-ALU source-times-mask
and full-RGBA lightmap schedules have the same packet length, so both retain
this cap.  If later state adds more than eight dwords beyond blend plus this
mip table, recompute rather than retaining 18 blindly.

The page proof follows the current builder, not the older PAUSE-only trailer.
`CHROME9_CR_READ_AHEAD=0x220` makes the last legal address `0xde0` in a 4-KiB
slot.  After the cookie, `chrome9_finish_ring()` uses a do/while to pad to a
128-byte PAUSE block, emits a 16-byte dummy header, then uses another
do/while to the next 128-byte STOP block.  Therefore the pre-padding cursor
must be at most `0xcf8`; a cursor exactly at `0xd00` advances PAUSE to `0xd70`
and STOP to the illegal `0xdf0`.  For a body of `L` dwords the cursor used in
the calculations above is:

```text
cookie_end = ALIGN(0x20 + 4*L, 16) + 0x68
```

Only per-vertex D0/D1/D2/D3 (and D4 for two textures) may vary inside the
first batch.  The destination BO/format/pitch/window, every source BO and
image layout, sampler wrap/filter/LOD/border state, shader and constants,
FVF/linkage, blend/alpha/depth state, color mask, scissor, viewport, topology,
and winding/cull convention are shared.  Flush on any such change.  Validate
each group of three for finite/range-safe attributes and nondegenerate area;
do not validate only the first triangle.

The smallest stable UAPI successor is a new structured batch ioctl, leaving
the existing fixed-three-vertex ioctls unchanged.  It can carry the shared
state once plus `triangle_count` and a 64-bit userspace pointer to an array of
`3*triangle_count` `drm_chrome9_textured_vertex` records.  Bound count to
1..24 before multiplication, use `memdup_array_user()` once before taking BO
locks, validate the copied snapshot, and free it on every exit.  A `__u64`
pointer decoded with `u64_to_user_ptr()` keeps the layout compat-stable.  The
builder should not place the maximum 2304-byte converted payload on the
kernel stack: either allocate it or write checked converted vertices directly
behind the already-bounded Type-00 header.  Set raster/FVF word 10 to the
dynamic A24 value and retain the exact index/body-length/PAUSE/STOP assertions
before publication.  The current submit path builds synchronously, so the
copied vertices need not survive job retirement; an asynchronous builder
would instead have to transfer ownership to the job.

The first live batching oracle is two REPLACE triangles forming the existing
64x64 textured quad.  Emit vertices `TL,TR,BR, TL,BR,BL`, set A24=`24000006`,
and use the red/green/blue/white 32-square nearest/clamp source.  Samples
`(8,8)/(48,8)/(48,48)/(8,48)` must be exactly
`00ff0000/0000ff00/00ffffff/000000ff`.  The last sample is owned only by the
second triangle and proves that the list continued past the first automatic
fire; full target/source guards and the unchanged source prove bounded fetch
and PLEND.  Follow it with the old one-triangle ioctl into a cleared target to
prove the dynamic A24 batch did not alter closure for the legacy path.

The first Mesa integration should keep a batch local to one
`chrome9_vbuf_draw_arrays()` or `chrome9_vbuf_draw_elements()` callback and
flush it before returning.  Gallium state and resource bindings are immutable
for that callback, while these functions already expand strips, fans, quads,
and quad strips to explicitly ordered independent triples.  Preserve that
order (including the odd-triangle strip swap), append only successfully
converted nondegenerate triples, duplicate shared vertices, and split at the
kind-specific kernel cap.  This obtains the large reduction in ioctl,
reservation, GART, cache, and fence work without needing pending-batch flushes
on later state changes, clears, maps, or context teardown.  In particular, do
not retain a first implementation across vbuf callbacks merely because the BO
handles happen to compare equal.

Validation remains per triangle even though shared draw-state translation can
move to callback setup.  The draw limit is also a triangle limit, not an ioctl
limit: restrict each chunk to the remaining
`CHROME9_DRAW_LIMIT-submitted_triangles`.  On successful batch submission add
the batch count to `submitted_triangles` and mark the render target written
once.  On submission failure none of its triangles retired, so account the
entire batch as rejected (and stop that callback rather than repeatedly
submitting to a possibly wedged engine).  Empty batches, including callbacks
whose triangles all collapse during fixed-point conversion, issue no ioctl.

The count and exact body/trailer length must be rejected **before any ring
write**.  The current builders fill the slot and only then call
`chrome9_finish_ring()`; relying on its final PAUSE/STOP bound check would let
an oversized variable payload overwrite beyond the 4-KiB slot before the
error is discovered.  Check the kind/state-specific formula above at builder
entry as defense in depth, in addition to the ioctl count cap, and bounds-check
the direct vertex emitter against that precomputed end.  A successful batch
otherwise uses the same BO lookup, reservation usage, map, GART binding,
single fence, cache ownership transitions, guard preparation/verification,
and error unwind as its corresponding single-triangle job.  Batch count does
not require additional BOs or fences.

### One-word RGBA-by-RGBA successor for game lightmaps

The alpha-mask program is not enough for fixed-function `GL_MODULATE` across
two texture units: ioquake3-style base-texture times RGB lightmap needs all
four components of stage 1.  The smallest scheduled derivative changes only
the first ALU's source-C swizzle at body dword 217:

```text
# vendor alpha-mask instruction
0140B6D0 302026DB 00F20005 0000007F   # r0.xyzw * r2.wwww

# RGBA-by-RGBA vendor instruction
0140B6D0 30202053 00F20005 0000007F   # r0.xyzw * r2.xyzw
```

All opcode, destination, dependency, register-allocation, and scheduling bits
are preserved.  Source C occupies dword 1 bits `[20:0]`; its exact identity
swizzle is low 12 bits `053` (`x=0,y=1,z=2,w=3`), while replicated W is
`6db`.  The second, output-MOV instruction and every TAU/control word remain
unchanged.  The complete body therefore remains 272 dwords.

This word is also emitted verbatim by the unmodified vendor helper
`Modulate_H5_src_comp_Aa_Ca2` (`via_drv.so:0x33290`) with mode 6.  An isolated
call of that helper produced the complete 40-dword control/TAU/ALU packet and
the scheduled instruction

```text
0140B6D0 30202053 00F20005 0000007F   # r0.xyzw * r2.xyzw
```

Mode 5 of the same helper emits the independently useful source-alpha form

```text
0140B6D0 B0202053 00F2006D 0000007F   # r0.wwww * r2.xyzw
```

The earlier `30202013` transcription was wrong; the helper's byte stores at
`0x3355d..0x338e2` and the direct call agree on `30202053`.  Separately, the
Linux proprietary PS 2.0
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

### Three-factor fixed-function successor

Compatibility fixed-function modulation can also consume primary vertex
color: `texture0 * texture1 * D1`.  The smallest bounded derivative of the
two-texture RGBA program adds one in-place MUL of physical `r1` by input
`v0`, then leaves the existing output MOV in place:

```text
0140B6D0 30202053 00F20005 0000007F   # r1 = r0 * r2
0140B6D0 30040053 00F20105 0000007F   # r1 = r1 * v0
01C00000 301C0000 00F20105 000D083F   # oC0 = r1
CCCCCCCC DDDDDDDD
```

Change the PS control count from `92020002` to `92020003`.  The second
instruction combines the exact input-v0 identity operand from the proven
one-texture MODULATE shader with the exact scheduled r1 identity source used
by the output MOV.  The proprietary Linux compiler independently accepts
`texld r0,t0,s0; texld r1,t1,s1; mul r2,r0,r1; mul r3,r2,v0; mov oC0,r3`
and emits two TAUs plus three ALUs, confirming the semantic operation and
count.  Its virtual registers and unscheduled dependency fields are not used
as packet words here.  The in-place scheduled instruction above remains a
bounded synthesis and must pass the guarded live test before exposure.

The headless Windows backend supplies a second, dynamically executed schedule
check without opening an adapter.  Compiling the equivalent direct-output
form

```text
texld r0,t0,s0
texld r1,t1,s1
mul   r0,r0,r1
mul   oC0,r0,v0
```

produces byte-identical raw and finalized stores:

```text
TAU 201001B0 211111B1
ALU 0140B6D0 00001000 00F00000 00000020
    0140B6D0 00040053 00F00000 00000820
A90..A9B:
    90000000 91000000 92020002 93000002
    94000000 95000000 96000000 97000000
    98000000 99000000 9A000000 9B000000
```

The finalized ALU and TAU SHA-256 values are respectively
`f23e3572537905862c0c9a3fcfb69159236c8476e8d2834825c7ace4002ea0f6`
and `503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`.
This proves that two independent fetches followed by two dependent ALUs need
only the same T2A switch value 2 and no A2T switch.  It does not replace the
packet words above: the probe's deliberately identity/zero allocator uses
virtual `r0/r1`, while the Xorg stream has physical `r0/r1/r2` dependencies
and retains its explicit output MOV.  Therefore the fixed successor still
uses ALU count three and `A93=93000002`; only a future general compiler may
legally lower this expression to the direct-output two-ALU form.

Keeping explicit temporaries and the final MOV in the same Wine probe closes
the fixed successor's count/control invariant more directly.  For
`mul r2,r0,r1; mul r3,r2,v0; mov oC0,r3` after the two TEXLDs, raw and final
stores are again identical:

```text
TAU 201001B0 211111B1
ALU 0140B6D0 00001000 00F00000 000000A0
    0140B6D0 00040053 00F00200 000000E0
    01C00530 001C0053 00F00300 000D0820
A92 92020003
A93 93000002
```

All remaining A90--A9B payloads are zero in the compiler's empty runtime
context.  The ALU hash is
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`;
the TAU hash is unchanged.  This independently proves the exact three-ALU
count and the unchanged two-TAU-to-ALU switch used by the fixed scheduled
packet, while its virtual register words remain non-substitutable.

This path also requires the two state changes which the source-only shader
could ignore: retain the live-proven FVF declaration `020041D1`, not the
Xorg mask stream's unnormalized `020041D0`, and use proprietary-GL default
type-01 A01=`01CE1540` so diffuse interpolation is perspective-correct.
Vertex stride stays ten dwords and D1 remains its word 4.

Keeping the dual-texture `CC/DD` terminators adds four dwords for the new
instruction.  With `H` mip-table packets before common state (`H=0,1,2`),
closed-blend delta `B=0 or 4`, and common-group D16 delta `D=0 or 4`, the
triangle body is `266 + 12*H + B + D` dwords.  D16 is inserted after A89, so
the earlier common A01 index does not receive `D`; every subsequent group
does.  Key indices are:

```text
common A01       = 64  + 12*H + B
FVF A02          = 86  + 12*H + B + D
PS A92           = 190 + 12*H + B + D
first MUL        = 216 + 12*H + B + D
second MUL       = 220 + 12*H + B + D
output MOV       = 224 + 12*H + B + D
vertex header    = 232 + 12*H + B + D
```

The state-complete single-level form with closed blend and D16 therefore has
`B=D=4`, totals **274 dwords**, places normalized D1 declaration A02 at 94,
and begins its vertex packet at 240.  Alpha test still uses the otherwise
aligned destination slot at body 19; it does not add a third delta.

For a single discriminator, reuse the RGBA two-texture quadrant vector and
give every vertex packed primary color `ff808080`.  The prior red/black/blue/
yellow product must become XRGB `00800000/00000000/00000080/00808000` within
normal one-LSB normalized-multiply rounding.  Ignoring D1 returns full-scale
colors; using FVF `d0` produces the already observed unnormalized saturation
failure.  Preserve both texture guards, target guards, finite RHW, and the
full-state one-stage transition after this test.

For the ioquake3-style overbright combination, apply result-scale x2 to the
**second**, final MUL:

```text
0140B6D0 30202053 00F20005 0000007F   # r1 = texture0 * texture1
0140B6D0 30040053 00F20105 0000207F   # r1 = (r1 * primary) * 2
01C00000 301C0000 00F20105 000D083F
CCCCCCCC DDDDDDDD
```

Only body dword `223 + 12*H + B + D` changes.  The first MUL's scale word at
`219 + ...` remains zero: placing the API's final combine scale there would
rely on reassociation and on the temporary never gaining stage-local clamp or
precision semantics.  Reuse primary `ff808080` and the red/black/blue/yellow
two-texture product.  Correct final x2 restores full-scale
red/black/blue/yellow, while no scale leaves nonblack results near half
intensity.  This vector proves x2 versus identity; because the current temp
write has saturation clear, it does not by itself distinguish an incorrectly
reassociated first-MUL scale.  Packet length, A92=`92020003`, transition bits,
and all resource state remain unchanged.

`GL_RGB_SCALE=4` is the adjacent proved native scale code 2, so the same
final word is `0000407F`; scale 1/2/4 therefore selects
`0000007F/0000207F/0000407F` with no other delta.  A scale-4 discriminator
uses packed primary `ff404040`: correct output again approaches the original
full-scale nonblack product, while identity remains near quarter intensity.
Do not expose native x8 merely because code 3 exists; fixed-function GL
texture environment accepts only 1, 2, and 4.

These one-word scale forms multiply alpha as well as RGB.  Gate them to the
opaque-lightmap case (both sampled alphas and primary alpha equal one) until a
separate RGB-only write plus unscaled-alpha write is scheduled and validated;
they are not a general implementation of independent `GL_RGB_SCALE` and
`GL_ALPHA_SCALE` for translucent material.

After that color-only test passes, combine it with the state-complete oracle
above: texture0=`80ff8040`, texture1=`80ffff80`, vertex D1=`ff808080`,
transparent-black BGRA destination, source-over, alpha LESS/ref `80`, and D16
LESS/write at Z=`4000`.  The three-factor fragment is approximately
`40804010`; correct post-test blending stores approximately
`40201004`/depth=`4000`.  Omitting primary color returns `40402008`, while a
failed alpha test leaves zero/`ffff`.  This is the final bounded interaction
gate before routing ordinary two-unit fixed-function GL draws through the
274-dword successor.

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

## Mesa two-texture integration audit

The current Gallium path cannot reach the packet above by changing only the
kernel ioctl.  Its NIR classifier, draw-vbuf layout, state validator, ioctl
marshalling, and advertised limits each encode exactly one texture.  The
smallest bounded Mesa successor is as follows; this deliberately keeps mip
levels, texture/RT aliasing, depth, and blended two-texture draws out of the
first capability.

1. Add a distinct `CHROME9_FS_TWO_TEXTURE_RGBA` program and a distinct kernel
   capability/ioctl.  Do not reinterpret either one-texture UAPI structure:
   its size and its eight-dword vertex are already ABI.  The new vertex is ten
   dwords in the hardware order
   `X,Y,Z,RHW,D1,D2,S0,T0,S1,T1`, and the new request carries two handles plus
   independent width, height, pitch, format, min/mag filter, and S/T wrap
   fields.
2. Replace `chrome9_fs_is_texcoord(def,depth)` with a unit-aware matcher for
   `VARYING_SLOT_TEX0 + unit`, and replace the Boolean texture matcher with one
   which returns the exact unit.  Require `texture_index == sampler_index ==
   unit`, unit 0 or 1, one 2D coordinate source, no array/shadow state, and the
   matching varying.  `chrome9_context.c:331..391` currently hard-codes unit
   zero in all three places.
3. Preserve factor identity while walking component-wise `fmul`.  A plain
   enum result is insufficient because it cannot distinguish `tex0*tex1`
   from `tex0*tex0`.  A three-bit factor mask (`T0`, `T1`, `D1`) is enough:
   pass identity `mov`/`fsat`, combine only disjoint masks, and map exactly
   `T0|T1` to the new program.  Track per-unit texture-instruction counts and
   require one instruction for each referenced bit and none for any other
   unit.  This also prepares, without exposing it, the later
   `T0|T1|D1` three-factor program.
4. In `chrome9_vbuf_get_vertex_info()` find GENERIC/TEXCOORD 0 and 1
   separately and emit two `EMIT_2F` attributes after the two packed colors.
   The exact two-texture size is ten dwords.  Require both coordinates and
   position; D1 may be absent only for pure `T0*T1`.  Change allocation and
   layout assertions from the present textured/nontextured `8/6` choice to an
   exact per-program `10/8/6` choice (`chrome9_draw.c:51..105`).
5. Generalize draw-state selection to two resource/sampler pairs.  For the
   new path require the highest bound sampler and view count to be exactly two
   **and** slots 0 and 1 both non-NULL/supported.  Validate each resource with
   the existing native guarded, aligned-data, canonical-pitch and level-zero
   rules.  Initially reject every pairwise alias by handle: target/texture 0,
   target/texture 1, and texture 0/texture 1.  Preserve the current rejection
   of depth and reject any state requiring the blend successor until a
   two-texture blend ioctl exists.  The current single-slot choke points are
   `chrome9_draw.c:326..342`.
6. Marshal vertex words 6/7 to S0/T0 and 8/9 to S1/T1, then populate both
   texture descriptions and submit only the new ioctl.  Add a matching thin
   wrapper in `chrome9_drm.[ch]`.  Do not silently fall back to two
   one-texture draws: multiplication is not representable by that sequence.
7. Gate all advertisement on the new kernel capability.  Set fragment
   `max_texture_samplers`, `max_sampler_views`, `max_tex_instructions`, and
   `max_tex_indirections` to two only with that bit; otherwise retain the
   current one.  The present limits are at `chrome9_screen.c:133..143`.
   Continue accepting only level-zero views: the current rejection at
   `chrome9_context.c:696..705` is correct until the mip-table ABI is live.

The first Mesa-side regression should use the same three raw-oracle pixels,
not a visually uniform quad.  Target is guarded 64x64 XRGB with pitch 256 and
sentinel `005a5a5a`; both sources are guarded 32x32 XRGB with pitch 128.
Texture 0 quadrants are red/green/blue/white and texture 1 quadrants are
red/red/blue/yellow.  At `(8,4)`, `(48,16)`, and `(56,48)` the exact expected
XRGB words are `00ff0000`, `00000000`, and `00ffff00`; outside at `(8,48)`
must remain `005a5a5a`.  Verify all six source/target guard pages and both
unchanged source images.  Only after that smoke test passes should the screen
advertise two samplers to the state tracker.

### Concrete first two-texture ABI contract

The initial structured request is deliberately fixed at 240 bytes.  It uses
ioctl number `0x0a` and capability bit 14, contains a 24-byte destination
prefix, two 36-byte texture descriptions, three 40-byte vertices, the common
12-byte scissor, and twelve reserved zero bytes.  The operation is implicit:
sample units 0 and 1 and multiply RGBA component-by-component.  There is no
program selector, mip pointer, blend state, destination-format selector, or
depth handle to accidentally expose unvalidated state.

Before acquiring reservations the ioctl must reject nonzero flags/reserved
words, a non-XRGB destination, non-POT or out-of-range source dimensions,
noncanonical pitches, unsupported formats/filter/wrap values, non-finite
RHW/coordinates, RHW at or below zero, and any S/T outside `[0,1]`.  Validate
the guarded object sizes with checked arithmetic and reject all three handle
aliases.  Each source gets its own independently derived A20/A22/A30/A31/A32
words; never copy stage 0's geometry or sampler word into stage 1.

The three-object lifetime is indivisible.  Reserve target, texture 0 and
texture 1 together; append their pages to the PTE array in that order; map,
guard, cache-flush and hand all three to the device before publication; and
attach the same completion fence to all reservations.  Normal retirement
returns DMA ownership and verifies both guards of every object before SF
unbind.  Any timeout or failed PTE invalidation quarantines the complete job,
including all three maps, GEM references, SG tables, PTEs and SF node.  It is
never safe to unwind texture 1 alone after the ring might have observed the
job.

`tools/chrome9_two_texture.c` is the first raw ABI oracle.  It fills the
target with `005a5a5a`, uses independent RGBW and red/red/blue/yellow source
quadrants, checks exact covered results `00ff0000/00000000/00ffff00`, checks
the outside sentinel, compares both complete source data pages, and verifies
all six guards.

### Structured ABI and Gallium silicon result (2026-09-21)

Kernel commits `98d175e` (UAPI) and `97e20ff` (Chrome9 DRM) implement the
240-byte request above as ioctl `0x0a`, capability bit 14.  The final builder
asserts the complete 262-dword body, uses three separately reserved GEM BOs,
and publishes through the already validated monotonic command ring.

The first live VX900 submission passed exactly:

```text
RGBA two-texture triangle passed: 00ff0000/00000000/00ffff00 outside=005a5a5a
caps=0x7ffd, bounded two-texture path passed
```

Both source images and all six guards were unchanged.  Two hundred consecutive
raw oracles completed in 1.26 seconds, crossing ring epochs without a timeout;
the previous one-texture REPLACE/MODULATE and variable-size D16 oracles then
remained exact.  After the complete raw and GL integration sequence, safe
quiesce reported `appends=558 wraps=17 epochs=17`; unload, reload, and another
two-texture oracle passed without rebooting.

Mesa commit `e985dfc` completes the GL1 state path.  Mesa's fixed-function NIR
retains a `nir_tex_src_projector` sourced from `TEXn.w` even for ordinary
`glMultiTexCoord2f`.  The bounded classifier now recognizes that exact form,
the draw module carries all four texture-coordinate components through
software T&L, and runtime validation accepts only finite coordinates with
`Q == 1`.  Real projective texturing remains rejected instead of being
silently reduced to S/T.  A surfaceless EGL test enabled both texture units,
generated the exact `tex0 * tex1` NIR, rendered through ioctl `0x0a`, and
passed 100 consecutive times.  The ten-frame three-gear D16/blend checksum
remained `f8b37bc9fe5e12b7` afterward.

Ordinary OpenGL uploads currently request a complete mip-capable resource
even when the active minification filter uses only level zero.  A 1x1 GL
texture therefore isolates and proves the full state/NIR/submission path;
larger GL textures require the separately tracked mip-capable resource-layout
work before this is a general application path.  The raw oracle already proves
independent D3/D4 interpolation for 32x32 sources.

### Mip-capable BO container validation (2026-09-21)

Mesa commits `120cdd9` and `4dbef4d` add a guarded linear mip container while
leaving the sampled level-zero contract unchanged.  Level zero still begins
after one page, and its page-rounded payload is immediately followed by the
original internal guard.  Levels 1 through `last_level` follow that guard at
256-byte-aligned offsets with 32-byte-aligned pitches, followed by a final
page guard.  Only a sampler view selecting exactly level zero and no mip
filter is eligible for the current ioctl; this resource work does not yet
claim hardware mip selection.

Kernel commit `1be84b1` maps the complete native texture BO, calculates every
following source address from actual preceding GEM sizes, and independently
prepares/verifies both the canonical level-zero guard and the final BO guard.
Destinations and D16 buffers retain exact-size validation.  The raw oracle's
`--mip-tail` mode allocates each 32x32 source as five pages: leading guard,
level-zero payload, internal guard, untouched synthetic mip-tail page, and
final guard.  This also forces texture 1's SF base to move by the enlarged
texture-0 allocation.

On the live VX900, 100 consecutive enlarged three-BO submissions plus a final
reported run produced the exact established quadrant samples:

```text
RGBA two-texture triangle with mip tails passed: 00ff0000/00000000/00ffff00 outside=005a5a5a
caps=0x7ffd, bounded two-texture path passed
```

Both internal guards, both final guards, both complete synthetic mip-tail
pages, both level-zero images, the destination guards, and the outside
sentinel remained exact.  This proves the larger-BO PTE span and inter-object
address arithmetic without conflating it with the still-unvalidated hardware
mip-table state.

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
