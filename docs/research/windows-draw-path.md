# Windows VX900 draw path and render-target color semantics

Status: static reverse engineering, cross-checked against the public VX900
manual, the VIA 5.75 Linux binaries, and the successful live 188-dword Linux
draw.  This note does not describe a new live submission.  PE addresses are
analysis virtual addresses for these exact files:

| file | SHA-256 |
|---|---|
| `VTGUModeDX32A.dll` | `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45` |
| `VTGOGL32A.dll` | `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c` |

The Linux comparison binary is `via_drv.so`, build ID
`2652605384b383d1377e040b8f8173b856e9a067`, SHA-256
`5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787`.

## Result

The Windows drivers confirm that a normal 7122 draw is not a fixed packet
blob.  API callbacks update a large hardware-state image and dirty bits.  The
draw callback gathers dirty ranges, serializes only those ranges, emits the
primitive, and clears the dirty records.  The Direct3D and OpenGL drivers use
the same state-image layout and nearly identical hardware routines.

The Windows H6 vertex emitter also independently confirms the corrected Linux
draw: type-04 subaddresses `61`, `62`, and `60` are emitted together before
the subtype-31 vertex program, and a subtype-32 constants packet accompanies
the program.  The old 160-dword Linux capture omitted live H6 linkage and
constant state; it was not a valid 7122 draw.

The successful Linux draw's blue result is now fully explained and is not a
red/blue render-target-format ambiguity.  The draw explicitly selects **solid
shading** and sets the solid color to opaque blue.  Its red per-vertex diffuse
words are therefore not the selected raster color.

## Direct3D DDI entry path

`OpenAdapter` at `0x1000105a` reaches `0x100922b0`.
`CreateDevice` at `0x100012df` reaches `0x10091920`; the constructor at
`0x10094680` copies 121 callback dwords from `0x1030dae8`.

The draw-relevant entries are:

| DDI operation | wrapper | implementation |
|---|---:|---:|
| SetRenderState | `0x10092a50` | `0x100b2dc0` |
| SetTextureStageState | `0x10092ae0` | `0x100b2fb0` |
| SetTexture | `0x10092b10` | `0x100b3030` |
| SetPixelShader | `0x10092b40` | `0x100b3140` |
| SetIndices | `0x10092bd0` | `0x100b3210` |
| DrawPrimitive | `0x10092c30` | `0x100b3360` |
| DrawIndexedPrimitive | `0x10092c70` | `0x100b3400` |
| DrawPrimitive2 | `0x10092cc0` | `0x100b34b0` |
| DrawIndexedPrimitive2 | `0x10092cf0` | `0x100b3540` |
| SetViewport | `0x10092f80` | `0x100b2e30` |
| SetZRange | `0x10092fb0` | `0x100b2e50` |
| SetVertexDeclaration | `0x100932e0` | `0x100b4370` |
| SetVertexShader | `0x100933c0` | `0x100b43e0` |
| SetScissorRect | `0x100934b0` | `0x100b2e90` |
| SetStreamSource | `0x100934e0` | `0x100b3280` |
| SetRenderTarget | `0x10093690` | `0x100b2eb0` |
| SetDepthStencil | `0x100936c0` | `0x100b2f40` |

`0x100b3360` constructs a 14-dword internal draw descriptor and calls
`0x1010f290`.  The DDI structure fields at `+0`, `+4`, and `+8` are primitive
type, start vertex, and primitive count respectively.  `0x1010f290` reaches
the hardware callback at table offset `+0x38`; for this image that entry is
`0x10156b20`.

The hardware callback reads the large state at context `+0x248`.  Unless the
manager field at `+0x268` forces type 1, it preserves the API primitive type.
Primitive type 1, D3D `POINTLIST`, takes the special path at `0x10153250`.
That large point emitter is therefore not the generic triangle draw oracle.
Triangles continue inline through the dirty-state gather and serializer.

The OpenGL ICD contains the corresponding routines at:

| purpose | Direct3D UMD | OpenGL ICD |
|---|---:|---:|
| normal primitive callback | `0x10156b20` | `0x102019a0` |
| point-list special path | `0x10153250` | `0x101fe110` |
| gather dirty state | `0x10150be0` | `0x101fc050` |
| clear emitted dirty state | `0x10150850` | `0x101fbcc0` |

These pairs have matching instruction bodies apart from image-relative calls
and addresses.  They use the same context offsets and state organization.

## Dirty-state serialization

`0x10150be0` does not directly emit a monolithic draw packet.  It gathers
dirty state into arrays in a command scratch object:

| scratch offset | recovered role |
|---:|---|
| `+0x2080` | number of pending ranges |
| `+0x0000` | per-range leading state/control word |
| `+0x0680` | source pointer in the hardware-state image |
| `+0x0d00` | H2/H3 parameter type or selector |
| `+0x1380` | range size/class |
| `+0x1a00` | byte offset within the selected state bank |

The normal primitive callback serializes these records into aligned H2/H3
packets.  `0x10150850` clears the emitted dirty records afterward.  This means
a Windows command capture depends on all preceding state calls; the absence of
a register in one draw normally means it was already resident, not that it is
optional for a fresh Linux context.

## H6 vertex-program prerequisites

The shared H6 VS-state emitters are at:

| binary | region |
|---|---:|
| Direct3D UMD | `0x1015ad00..0x1015aff4` |
| OpenGL ICD | `0x102052e0..0x10205714` |

They construct three consecutive type-04 words in the hardware-state image:

- `+0x2260`, subaddress `60`: bit 23 valid and compiler metadata `+0x14 - 1`
  in bits 8:0.  Hand-built rectangle programs make this equal to instruction
  count minus one, but generic compilation can upload inactive tail records;
- `+0x2264`, subaddress `61`: bit 23 for the small-resource case, bits 22:20
  from compiler object `+0x08`, and bit 19 from object `+0x0c`;
- `+0x2268`, subaddress `62`: low nine bits from object `+0x10`.

The emitter writes the H2 words in the exact order `61`, `62`, `60`, then an
H3 subtype-31 program.  Before that it emits H3 selector `32030400`, subtype
32, for VS float constants.  Consequently `60800003` alone is not a complete
VS activation sequence.

The D3D disassembly also proves that the two VS length concepts are separate.
At `0x1015ac64..0x1015ac9a` it retains `object+0x424` as the upload-record
count while installing `object+0x14-1` into `60[8:0]`; the former sizes both
the H3 packet and memcpy at `0x1015af53..0x1015afa8`.  This matches the Linux
uploader exactly and prevents treating inactive compiler tail records as live
instructions.

The corrected Linux Xorg stream contains the matching H6 state
`61800000 622081ff ... 60800003`, the subtype-32 constants, subtype-33 integer
constants, and a four-instruction VS.  Its exact 188 dwords are in
[vp-stall-analysis.md](vp-stall-analysis.md).

The complete upload order, VS/PS varying linkage, compiler limits, and the
smallest packet-exact constant-color derivative are collected in
[shader-upload-oracles.md](shader-upload-oracles.md).

## Exact meaning of `52980004`

This word is HParaType 01, subaddress `52`, render-target-0 setting 3:

```text
52 98 0004
   ^^ ^^^^
   |  pitch = 4 * 32 bytes = 128 bytes
   format byte
```

The Chrome9 HD manual defines format bits 23:19 as a family and bits 18:16 as
the member of that family.  The relevant byte-level enumeration is:

| byte/code | logical format | packed memory bits |
|---:|---|---|
| `15` | AL88 | A8 L8 |
| `16` | L16 | L16; post-rendering disabled |
| `30` | packed YUV / YUY2 | V/Y1/U/Y0 in descending bytes; paired pixels share U/V |
| `88` | RGB555 | bit 15 unused, then R5 G5 B5 |
| `89` | RGB565 | R5 G6 B5 |
| `8a` | ARGB1555 | A1 R5 G5 B5 |
| `8b` | ARGB4444 | A4 R4 G4 B4 |
| `98` | ARGB0888 / X8R8G8B8 | `xxxxxxxx RRRRRRRR GGGGGGGG BBBBBBBB` |
| `99` | ARGB8888 / A8R8G8B8 | `AAAAAAAA RRRRRRRR GGGGGGGG BBBBBBBB` |
| `9a` | ARGB2:10:10:10 | `AA RRRRRRRRRR GGGGGGGGGG BBBBBBBBBB` |
| `a8/a9/aa/ab` | BGR555, BGR565, ABGR1555, ABGR4444 | B and R exchanged relative to `88..8b` |
| `b8` | ABGR0888 / X8B8G8R8 | `xxxxxxxx BBBBBBBB GGGGGGGG RRRRRRRR` |
| `b9` | ABGR8888 / A8B8G8R8 | `AAAAAAAA BBBBBBBB GGGGGGGG RRRRRRRR` |
| `ba` | ABGR2:10:10:10 / A2B10G10R10 | `AA BBBBBBBBBB GGGGGGGGGG RRRRRRRRRR` |
| `bb` | G16R16 | `GGGGGGGGGGGGGGGG RRRRRRRRRRRRRRRR` |
| `c8/c9/ca/cb` | RGB555, RGB565, RGBA1555, RGBA4444 | R high; A is the low bit/nibble when present |
| `d8/d9/da` | RGBA8880, RGBA8888, RGBA10:10:10:2 | R high, A low |
| `e0/e1/e2/e3` | BGR555, BGR565, BGRA1555, BGRA4444 | B high; A is the low bit/nibble when present |
| `e8/e9/ea` | BGRA8880, BGRA8888, BGRA10:10:10:2 | B high, A low |
| `f0/f4/f5` | R16F, two-channel 16F, R32F | floating formats; post-rendering disabled |

The family bases from bits 23:19 are `88` ARGB16, `98` ARGB32, `a8` ABGR16,
`b8` ABGR32, `c8` RGBA16, `d8` RGBA32, `e0` BGRA16, `e8` BGRA32, and `f0`
floating color.  The manual contains several apparent labeling/field typos in
this area: it labels the BGRA16 description `ABGR_16bpp`; calls the low two
bits of RGBA10:10:10:2 and BGRA10:10:10:2 `R`; and calls BGRA8888's low byte
`R`.  The format names and the ordering of every adjacent family member imply
that these low fields are alpha.  That correction is a strong table-structure
inference, not yet an independent silicon test of the three typo-affected
formats.  It does not affect code `98`.

For the tested code `98`, the interpretation is not ambiguous:

```text
logical red   -> 0x00ff0000 in memory
logical green -> 0x0000ff00 in memory
logical blue  -> 0x000000ff in memory
```

The VIA 5.75 XRender format table independently maps `x8r8g8b8` to `980000`,
`a8r8g8b8` to `990000`, `x8b8g8r8` to `b80000`, and `a8b8g8r8` to `b90000`.
The older open-source VIA register header gives the same relative destination
codes as `ARGB0888=8`, `ARGB8888=9`, `ABGR0888=a`, and `ABGR8888=b`.

### Exact Windows API-to-hardware conversion

The Direct3D UMD supplies a direct, two-stage proof of code `98`, rather than
merely agreeing with the manual.  `0x100c7590` converts public D3D formats to
the driver's private surface-format enum:

| D3D9 format value/name | private format |
|---:|---:|
| `21` A8R8G8B8 | `2030` |
| `22` X8R8G8B8 | `2031` |
| `23` R5G6B5 | `1018` |
| `24` X1R5G5B5 | `101a` |
| `25` A1R5G5B5 | `1019` |
| `26` A4R4G4B4 | `101b` |
| `31` A2B10G10R10 | `203b` |
| `32` A8B8G8R8 | `2038` |
| `33` X8B8G8R8 | `2039` |
| `34` G16R16 | `203c` |
| `35` A2R10G10B10 | `203a` |
| `36` A16B16G16R16 | unsupported (`0`) |
| `51` A8L8 | `1029` |
| `81` L16 | `1028` |
| `111` R16F | `102c` |
| `112` G16R16F | `203d` |
| `114` R32F | `203e` |
| FOURCC YUY2 | `1023` |

The dedicated render-target lookup at `0x1020a4c0` reads that private value
from surface offset `+0x34` and returns the destination byte:

| private format | destination byte |
|---:|---:|
| `1017`, `101a` | `88` |
| `1018`, `1019`, `101b` | `89`, `8a`, `8b` |
| `1023` | `30` |
| `1028`, `1029`, `102c` | `16`, `15`, `f0` |
| `2030`, `2031` | `99`, `98` |
| `2038`, `2039` | `b9`, `b8` |
| `203a`, `203b`, `203c` | `9a`, `ba`, `bb` |
| `203d`, `203e` | `f4`, `f5` |
| anything else | `ffffffff` |

Thus the executable mapping is exactly `D3DFMT_X8R8G8B8 (22) -> 2031 ->
98`.  The OpenGL ICD has the byte-identical render-target lookup at
`0x1025d3f0`; its independently identified symbol-equivalent is documented as
`stmLookupRenderTargetFormat_invi` in [gl-feature-matrix.md](gl-feature-matrix.md).
Do not substitute D3D `0x1020a660` or OpenGL `0x1025d590`: those are broader
texture-format lookups and accept sampler formats that are not color targets.
The uncommon chains are equally explicit: `A8L8 -> 1029 -> 15`, `L16 ->
1028 -> 16`, `YUY2 -> 1023 -> 30`, `R16F -> 102c -> f0`, `G16R16F -> 203d
-> f4`, and `R32F -> 203e -> f5`.  The manual disables post-rendering for L16
and floating color, so those mappings do not by themselves establish blending.
Its `f4` prose also contradicts the `G16FR16F` name about which half contains
R versus G; retain the API-format identity but treat the halfword order as
unverified until a store/readback test or another independent path resolves
that typo.

The D3D RT state builder at `0x101f9f90` calls the RT-only lookup at
`0x101fa11b`.  For RT index `n`, its four state words begin at
`0x140 + n*0x20` in the hardware image and correspond to subaddresses
`50 + n*8` through `53 + n*8`.  For RT0 it:

- inserts `surface[+0xf8] >> 8` into image word `+0x140` (subaddress `50`,
  relocated base address);
- updates layout bits in `+0x144` (subaddress `51`);
- writes the lookup result byte to `+0x14a`, bits 23:16 of aligned image word
  `+0x148`, and inserts `(surface[+0x160] & 0x3fff) >> 5` into that word's low
  ten bits (subaddress `52`, format plus pitch);
- dirties state indices `50..53` for serialization.

This is the exact construction of `52980004`: destination code `98` in bits
23:16 and a 128-byte pitch (`4 * 32`) in bits 9:0.  RT1/2/3 use the identical
sequence at image offsets `+0x160/+0x180/+0x1a0`.

## Write mask and shader output mapping

`53010c0f` is render-target-0 setting 4:

- bits 16:15 are `10`, rounding with multiplication by the destination range;
- bits 11:8 are `1100`, `COPY_PEN`, so the new pixel color is written;
- bits 3:0 are `1111`, enabling alpha, red, green, and blue writes;
- sRGB/de-gamma fields are zero.

There is no channel swap in this word.  In particular, the low mask bit is
blue and all four mask bits are set.

The Direct3D state path proves the mask ordering independently.  D3D render
state `168`, `D3DRS_COLORWRITEENABLE`, reaches the case at `0x100ca8d3`, is
stored at context `+0x1e70`, and marks internal group `45` dirty.  Render
targets 1 through 3 use the following dwords at `+0x1e74/+0x1e78/+0x1e7c`.
The RT mask builder at `0x101f9cd0` reads `context[0x1e70 + 4*n] & 0xf` and
performs exactly:

```c
hw_mask = ((d3d_mask & 0x1) << 2) | /* D3D R -> hardware bit 2 */
          ( d3d_mask & 0x2)       | /* D3D G -> hardware bit 1 */
          ((d3d_mask & 0x4) >> 2) | /* D3D B -> hardware bit 0 */
          ( d3d_mask & 0x8);        /* D3D A -> hardware bit 3 */
```

It merges the result into the low nibble of hardware-image word `+0x14c +
0x20*n`, subaddress `53 + 8*n`, and dirties precisely that state index.  The
context initializer at `0x100c58c6` sets every available RT API mask to `f`.
Thus live low nibble `f` is not merely a manual decode: it is the executable
result of D3D's `RGBA=1111` after the driver's explicit `RGBA -> ARGB`
bit-position conversion.  An all-channel mask is invariant under that
permutation, but individual-channel masks are not.

The H6 declaration word `020041d0` maps packed D1 color bytes as
`R=[23:16], G=[15:8], B=[7:0], A=[31:24]`.  Therefore vertex word
`ffff0000` supplies red and alpha byte values ff.  Its format bit 0 is clear,
however, so those bytes are not divided by 255; the correct normalized
D3DCOLOR declaration is `020041d1`.  The full-value oracle still saturates to
opaque red, which is why it did not reveal the error.  The vertex program's
third instruction is `MOV oD0,v1`; it does not exchange red and blue.

The pixel instruction is:

```text
01c00000 301c0000 00f04005 000d083f
```

The independently recovered compiler fields decode source A as input class 1,
id 0 (`v0`), destination mask `f` (XYZW), and destination class 2, id 0
(`oC0`).  Dword 3's suboperation is `d`, `MOV`.  It is therefore a full-mask
`MOV oC0,v0`, again with no component permutation.  The manual then packs
logical `oC0` components according to subaddress 52's destination format.

## Why red vertices produced `000000ff`

The decisive words in the verified stream are:

```text
860000ff 870000ff 23000f02
```

For HParaType 01 subaddresses `86/87`, the manual defines:

```text
86[23:16] = solid R = 00
86[15:8]  = solid G = 00
86[7:0]   = solid B = ff
87[7:0]   = solid A = ff
```

Thus the programmed solid color is opaque blue.  In type-04 subaddress `23`,
word `23000f02`, shading bits 14:12 are `000`, which the manual defines as
solid shading.  Bits 11:9 are `111` (all triangle edges) and bits 3:0 are
`0010` (triangle).  The old open-source Linux/Mesa header independently names
the zero shading value `HC_HShading_Solid` and `0x1000` as Gouraud.

The resulting path is therefore:

```text
red D3DCOLOR vertices -> VS oD0       (valid but not selected for shading)
solid state ff0000ff  -> raster v0    (opaque blue)
PS MOV v0 -> oC0      -> RT 98 pack   -> memory 000000ff
```

This exactly predicts the measured 1,024 pixels of `000000ff`.  Endianness,
the destination format, the write mask, the VS, and the PS do not explain a
red/blue exchange because none performs one.

Two one-dword discriminators are available for a later guarded test:

1. Safest packed-color test: change only `860000ff` to `86ff0000`.  Keep solid
   shading.  The predicted memory result is `00ff0000`.
2. Vertex-color-source test: leave solid blue in `86/87`, but change only
   `23000f02` to `23004f02`, selecting Gouraud (`100` in bits 14:12).  With all
   four vertices `ffff0000`, the predicted result is again `00ff0000`.

Neither change alters a packet count, address, vertex count, or shader length.
The first is the smaller semantic change and should precede the second.

## H6 topology, shading, clipping, and list termination

Three type-04 words completely describe the primitive list once the FVF and
vertex shader are resident:

| word | relevant fields |
|---:|---|
| `20` | even-cycle vertex reuse `[21:16]`, first/odd-cycle reuse `[15:8]`, index width `[1]`, command/index mode `[0]` |
| `23` | shading `[14:12]`, edge flags `[11:9]`, back-face selector `[8]`, primitive type `[3:0]` |
| `24` | exact number of vertices or indices in the current primitive list `[23:0]` |

For immediate, non-indexed triangles the useful A20 values are:

| topology | complete A20 word | first/odd reuse byte | even reuse field |
|---|---:|---|---|
| triangle list | `20000000` | full three-vertex cycle | full three-vertex cycle |
| triangle strip | `201c7800` | automatic; `A=C, B=B, C=new` (`78`) | `A=A, B=C, C=new` (`1c`) |
| triangle fan | `201c5c00` | automatic; `A=A, B=C, C=new` (`5c`) | `A=A, B=C, C=new` (`1c`) |

The reuse fields are not guessed GL enums.  The public manual defines the
three two-bit A/B/C selectors and the automatic-cycle bit.  The old Mesa
`viaRasterPrimitive()` constructs exactly these strip and fan cycles from
`HC_HVCycle_*`.  The Windows topology dispatcher at D3D `0x101f7410` reads
the D3D primitive type from context `+0x3e0c` and dispatches point list, line
list, line strip, triangle list, triangle strip, and triangle fan to
`0x101f4eb0`, `0x101f4f50`, `0x101f5140`, `0x101f5350`, `0x101f56d0`, and
`0x101f5aa0`.  Its byte-identical OpenGL copy is at `0x1024be90`, with the six
builders at `0x10249930`, `0x102499d0`, `0x10249bc0`, `0x10249dd0`,
`0x1024a150`, and `0x1024a520`.

The Windows triangle-list builder clears A20 bits `[21:8]`.  The internal
selector-8 strip path installs `001c7800` at `0x101f5985..0x101f5997`, while
the fan builder installs `001c5c00` at `0x101f5d58..0x101f5d68`.  Windows
also has an older/different strip sequence, `00386300`, behind the other
internal-selector branch; do not use that sequence for the documented H6
command-mode path.  Most importantly, the live VX900 Xorg quad independently
used `201c5c00` and completed on silicon.

### Shading selector

Keeping every non-shading bit from the successful triangle word gives:

| shading | A23 word |
|---|---:|
| solid | `23000f02` |
| flat from A | `23001f02` |
| flat from B | `23002f02` |
| flat from C | `23003f02` |
| Gouraud | `23004f02` |

The low `2` selects triangles, `e00` enables all three edge flags, and `100`
is the already-proven back-face selector; only `[14:12]` changes in this
table.  Both proprietary builders read D3D `SHADEMODE` from context `+0x1e8c`
and write `4` for Gouraud.  Their selector-8 triangle path uses FlatC for D3D
flat shading, agreeing with old Mesa's `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`,
and `GL_TRIANGLE_FAN` handling.  Flat provoking-vertex selection varies in
the driver's alternate topology branches, so a new driver should use
Gouraud first and implement flat shading only together with the selected
cycle/winding convention.

### Color window and scissor

The normal 12-bit clip words are:

| type-01 word | `[23:12]` | `[11:0]` |
|---:|---|---|
| `80` | color-window top | color-window bottom |
| `81` | color-window left | color-window right |
| `82` | scissor top | scissor bottom |
| `83` | scissor left | scissor right |

`SetViewport` at `0x100c99f0` stores `x, y, width, height` at context
`+0x1f00..+0x1f0c`.  `SetScissorRect` at `0x100c9c00` stores the public RECT
as `left, top, right, bottom` at `+0x3e14..+0x3e20`.  The shared builder near
`0x101f7949` intersects the enabled scissor with the drawable/viewport and
packs the result into hardware-image offsets `+0x200..+0x20c`, then dirties
all four words.  For the verified 32 by 32 target it emitted:

```text
80000020 81000020 82000020 83000020
```

Thus right and bottom are the exclusive D3D RECT ends (`32`), not `width-1`
or `height-1`; both color and scissor windows cover `[0,32) x [0,32)`.  The
VX900 manual allows coordinates through 2048.  Empty intersections should be
discarded in software until the zero-area hardware behavior is tested.

The same normal viewport builder also owns the independent pixel-Z clip
interval.  At `VTGUModeDX32A.dll:0x101f8bb4..0x101f8c0e` the no-user-clip
path loads near `ff000000` and far `7f000000`, splits the low 24 bits into
type-01 A1F/A1E and their high bytes into A20, and produces exactly:

```text
1E000000 1F000000 20007FFF
```

The byte-identical Linux DRI builder is
`via_chrome9_dri.so:0x1bb1e0..0x1bb292`.  The manual applies this check before
depth testing: pixels with `Z > far || Z < near` are dropped.  Consequently
the indexed-reset A20 value `20000000` is not a harmless default: together
with reset A1E/A1F it selects the degenerate interval `[0,0]`.  A Z=0 oracle
can pass while every ordinary positive depth is silently discarded.  A new
driver must emit at least `20007fff` before its first nonzero-Z draw; A1E and
A1F may retain their zero-low24 values for this wide-open interval.  The
initial live A20-only experiment was confounded by stale DMA bounce-buffer
contents overwriting the userspace depth initialization; see the corrected
ownership sequence in the depth section below before interpreting that run.

The live viewport/C-to-screen block should initially be treated as one
indivisible prerequisite:

```text
40800000 41400000 4200493F
43800000 44400000 4500493F
46800000 47400000 4800003F 49000000
50000000 51000000 52147FFF
```

A40/A41 plus A42's low/high bytes form X scale `3f800000` and offset
`49400000`; A43/A44/A45 do the same for Y.  A46/A47/A48 contain Z scale
`3f800000` and offset `00400000`, with A48's top byte the area-exponent
threshold.  A49 is reserved zero.  A50/A51/A52 contain upper/lower clamp
values `7f000000`/`ff000000` and threshold byte `14`.  The matching VS
constants are c256 `(1,1,1,1)` and c257 `(786432,786432,0,0)`.  These values
are proven as a set; the unusual Z offset and the exact generalized viewport
formula still need isolated tests before replacing them with computed state.

### Exact immediate count and termination

The manual is explicit about the Type-00 protocol: the command regulator
forms vertices according to the FVF length, fires automatically whenever a
complete primitive is ready according to A20/A23, and generates PLEND when
the A24 vertex count is exhausted (or upon a separate stop command).  There
is no explicit fire or list-end dword after a complete H6 immediate payload.

For the verified declaration, A00 is `00000206`, whose low count is six
dwords per vertex, and A24 is `24000004`.  Therefore the final Type-00 packet
contains exactly `4 * 6 = 24` payload dwords.  For an arbitrary command-mode
count `n`:

```text
A24 = 0x24000000 | n
payload dwords = n * 6
```

The vendor emitters align every H2 group to 16 bytes.  Since the Type-00
header is four dwords and this FVF has a six-dword stride, an even `n` needs
no trailing padding and an odd `n` needs two `CC000000` padding dwords.  The
padding is outside the A24-counted vertex payload.  Validate list counts in
software: triangle lists require `n % 3 == 0`, while strips and fans require
`n >= 3`.  Three vertices do not exercise the strip/fan reuse cycle; use at
least four to validate `78/5c` and `1c` reuse.

The API primitive-count conversion is consequently point list `n=p`, line
list `n=2p`, line strip `n=p+1`, triangle list `n=3p`, and triangle strip/fan
`n=p+2`.  Index mode changes A20 bit 0 and makes the Type-00 payload indices;
it is deliberately outside this first immediate-mode oracle.

### Minimal colored-triangle oracle

The smallest-risk three-vertex successor starts from the silicon-verified
188-dword body in [vp-stall-analysis.md](vp-stall-analysis.md), keeps its
fan cycle, and makes only these state changes:

```text
23000F02 -> 23004F02  # solid -> Gouraud
24000004 -> 24000003  # four vertices -> three
```

Replace the old final 28 dwords with these 24 dwords:

```text
FE020440 0000043C 00000000 CC000000
BF000000 BF000000 00000000 3F800000 FFFF0000 FFFF0000
41FC0000 BF000000 00000000 3F800000 FF00FF00 FFFF0000
41FC0000 41FC0000 00000000 3F800000 FF0000FF FFFF0000
CC000000 CC000000
```

This produces a 184-dword body.  The three D1 colors are opaque red, green,
and blue; the unchanged VS copies D1 to oD0 and the unchanged PS copies the
raster color to oC0.  The geometry is the first triangle of the proven quad,
so it preserves the successful winding and covers half of the 32 by 32
target.  This derivative is packet- and state-exact static evidence but has
not yet been executed on silicon.

After that succeeds, change only `201c5c00 -> 20000000`.  A three-vertex fan
and a three-vertex list have identical geometry, so an identical image is the
safest discriminator for the triangle-list cycle.  To exercise reuse with a
four-vertex quad, use TL,TR,BR,BL with `201c5c00` for a fan, or TL,TR,BL,BR
with `201c7800` for a strip.  This separates count/shading validation from the
more wedge-prone reuse transition.

## Safest front-end discriminator after the old VP stall

For a context which has not yet run the verified draw, do not begin with
partial vertex payloads.  The type-04 state says four six-dword vertices and
the front end can wait indefinitely for an incomplete primitive.

The smallest packet-shape-supported address-free front-end test is now the
72-dword H6 state core documented in
[vp-stall-analysis.md](vp-stall-analysis.md): the corrected general block, a
short Windows-proven type-04 transaction containing only the H6
declaration/linkage words, and the complete constants/VS/control groups, with
no vertices.  Its first 44 dwords through the constants form an even safer
activation-free first stage.  The earlier 92-dword form, which keeps the full
Xorg type-04 block, remains the conservative fallback.  Bracket either with
the proven 2D-to-3D sync and status packets.  The two color discriminators
above apply only to the already-complete 188-dword draw; they are not
substitutes for the prerequisite gate.

`0400=00080031` decomposes as the command regulator, vertex processor, overall
3D busy, plus bit 19, while downstream clip/setup/raster/pixel units are idle.
That is consistent with the rejected mixed H5/H6 vertex setup.  The corrected
188-dword stream retired with `0400=0`; the Windows `61/62/60` tuple supplies
independent confirmation of the missing state.

## Adjacent formats, depth, textures, and queries

The current exact Linux render-target table is recorded in
[via-5.75-dri-state.md](via-5.75-dri-state.md).  Besides the four 32-bit formats
above it proves RGB/BGR 555/565 and ARGB/ABGR 1555/4444 destinations.  It does
not prove blending, multisampling, tiling, or filtering for every listed
texture format.

The Windows D3D conversion and RT-only lookup above now prove the complete
API-to-state chain for the common color targets.  The RT validator at
`0x100c86c0` accepts private formats `1017..101b`, `1023`, `1028/1029`,
`2030/2031`, `2038..203e`, and `2044`; some additional cases (`0815`, `080a`,
`102a`, and `102c`) are capability-conditional.  Acceptance is not proof that
blend, multisample, or post-rendering works on every format, especially the
float codes for which the manual explicitly disables post-rendering.

The Direct3D SetRenderTarget path is
`0x10093690 -> 0x100b2eb0 -> 0x100cd010`.  It resolves a resource/subresource,
tracks up to four RT bindings in the hardware context around `+0x3d88`, and
marks the corresponding state dirty.  `0x100cd010` also classifies surface
layout flags masked by `0xfc00` into internal cases 0 through 5 for values
`0400,0800,1000,2000,4000,8000`.  Those are resource-layout classes, not the
subaddress-52 color enum, and should remain opaque until their resource
constructor is traced.

### Depth and stencil: exact Windows state path

The normal D3D render-target/depth builder is at `VTGUModeDX32A:0x101f9f90`.
Its first 256 bytes occur verbatim in `VTGOGL32A:0x1024dc50`; the depth-format
lookup at `0x1020a600` is byte-identical to the OpenGL lookup at `0x1025d530`.
The depth resource is the binding at context `+0x3de8`; the fields used by this
path are GPU base `+0xf8`, byte pitch `+0x150`, and layout flags `+0x2c`.

| type-01 word | Windows construction | hardware meaning |
|---:|---|---|
| `10` | `(resource[+f8] >> 8) & 00ffffff` | Z/ZW base, 256-byte units |
| `11` | `(resource[+150] << 8) & 007fe000`, plus location `[1:0]` | pitch `[22:13]`, 32-byte units in linear mode |
| `12` | `((format | 38) << 16)`, tile bit from `(flags >> 1) & 1` | format `[18:16]`, clamp/force bits `[21:19]`, layout bit 0 |
| `13` | `(D3DCMP - 1) << 16` | Z function `[18:16]` |

Thus the pitch expression is exactly equivalent to
`(byte_pitch >> 5) << 13`; it is not a byte-pitch field.  The builder normally
sets all three documented fixed-point safety controls: force Z=1 to 1-epsilon,
clamp values over one, and clamp negative values to zero.  It leaves the Z/W
selector at bit 23 clear.  A separate-stencil allocation at resource `+0x2e0`
adds subaddress `28` with another `(gpu_base >> 8)` value and uses subaddress
`12[6:5]` for its location.  Resources at `+0x2dc` and adjacent fields build
the optional coarse-Z `29/2a` words.  Those optional resources, Z caches, and
fast clear are unnecessary for the first plain linear-Z draw.

Depth state is not complete without the pixel-Z clip range.  The Direct3D
viewport builder at `VTGUModeDX32A:0x101f7570` constructs the normal fallback
bounds at `0x101f8bb4..0x101f8c0e`.  The OpenGL copy starts at
`VTGOGL32A:0x1024bf90` and performs the identical writes at
`0x1024d5e9..0x1024d643`.  Both select broad finite IEEE bounds with bit
patterns `ff000000` and `7f000000`, split their low 24 bits into hardware
image offsets `+0x7c/+0x78`, and their high bytes into `+0x80[7:0]` and
`+0x80[15:8]`.  Since those image offsets are type-01 A1F, A1E, and A20, the
exact normal words are:

```text
1E000000  # far low 24 bits
1F000000  # near low 24 bits
20007FFF  # far high byte 7f, near high byte ff
```

The public manual says this range check occurs before the depth test and drops
a pixel when `Z > far || Z < near`.  Therefore the indexed-reset value
`20000000`, combined with zero A1E/A1F payloads, is not a harmless default: it
makes both bounds zero.  This statically explains how Z=0 can pass while an
ordinary positive Z is clipped, but the first live A20-only run was not a
valid discriminator: a following degenerate-triangle run showed that the
entire D16 target had already changed from userspace's `ffff` to `0000`.
That was subsequently traced to a pre-submit DMA ownership error, not raster
coverage or a hardware depth clear.  Retest the clip interval only after the
correct CPU-to-device transition described below.  A self-contained first
depth packet must still emit at least `20007fff`; emitting all three words is
the conservative Windows-exact transaction.

The failure signature is important for future bring-up.  GEM creation eagerly
maps the shmem pages for bidirectional DMA while their contents are zero.  On
the 32-bit/SWIOTLB path, a later `dma_sync_sgtable_for_cpu()` before submission
copies those stale bounce-buffer zeros over the `ffff` values written through
the userspace mmap.  The fixed single-BO oracle did not expose this because it
initialized depth in the kernel after that sync.  The correct lifetime is:

1. sync for CPU immediately after the eager DMA map and before exposing the
   object to userspace;
2. on submission, flush CPU aliases and sync for device, with no intervening
   sync-for-CPU while userspace owns the object;
3. sync for CPU once after proven GPU retirement, before readback.

No current depth word requests a full-surface clear: A14/A15 only contain
clear data, A27 bit 0 is the documented fast-Z-clear enable, and the packet
leaves A27 disabled.  A degenerate primitive changing every sample is thus an
ownership diagnostic, not evidence for an implicit clear operation.

This state is independent of vertex layout and shading.  Windows changes no
H6 declaration, A23 shading selector, A24 count, or immediate vertex stride
when depth is enabled.  The proven Gouraud triangle remains
`A23=23004f02`, `A24=24000003`, with six dwords per vertex: float4 D0
`x,y,z,w`, packed D1 diffuse, and packed D2 specular.  The captured
`020041d0 030052d0` declaration leaves both COLOR values unnormalized; use
`020041d1 030052d1` when the packed values represent normalized API colors.

The public D3D-format chain is now resolved end to end:

| public D3D format | private format | `12[18:16]` | manual meaning |
|---|---:|---:|---|
| `D16_LOCKABLE` (70), `D16` (80) | `101d` | `0` | 16-bit fixed Z |
| `D32` (71) | `203f` | `4` | 32-bit fixed Z |
| `D24X8` (77) | `2032` | `6` | 24-bit fixed Z in `[31:8]` |
| `D24S8` (75) | `2033` | `6` | 24-bit fixed Z plus S8 in `[7:0]` |
| `D32F_LOCKABLE` (82) | `2040` | `5` | 32-bit `s[8].23` floating Z |

`D15S1` and `D24X4S4` route to the unsupported/assert path in this build.
The `ZENABLE`, `ZWRITEENABLE`, and `ZFUNC` public render-state cases at
`0x100ca38a`, `0x100ca3fc`, and `0x100ca420` store context fields
`+0x1d40`, `+0x1d44`, and `+0x1e94`.  Builder `0x101fa910` enables type-01
subaddress `01` bit 3 only when a valid depth binding exists and Z is enabled;
it adds bit 2 when Z writes are enabled.  Otherwise it clears `01[4:2]`, also
disabling stencil.  Builder `0x101faa10` maps all eight `D3DCMPFUNC` values by
subtracting one, exactly matching the manual's never/less/equal/LE/greater/
not-equal/GE/always encoding.  The latter builder is byte-identical at
`VTGOGL32A:0x1024e6d0`.

Stencil builder `VTGUModeDX32A:0x101fc060` is byte-identical to
`VTGOGL32A:0x1024fa10`.  It enables `01[4]` only when the public
`STENCILENABLE` state is true and the bound private format is `2033`.  Its
normal clockwise packing is:

| word | field | public D3D state/context field |
|---:|---|---|
| `23[23:16]` | reference | `STENCILREF`, `+1eac` |
| `23[15:8]` | compare mask | `STENCILMASK`, `+1eb0` |
| `23[7:0]` | write mask | `STENCILWRITEMASK`, `+1eb4` |
| `24[18:16]` | function | `STENCILFUNC - 1`, `+1ea4` |
| `24[8:6]` | stencil-fail op | `STENCILFAIL - 1`, `+1e9c` |
| `24[5:3]` | Z-fail op | `STENCILZFAIL - 1`, `+1ea0` |
| `24[2:0]` | pass op | `STENCILPASS - 1`, `+1ea8` |
| `25[23:16]` | reference | same reference, `+1ec8` |
| `25[15:8]` | compare mask | same mask, `+1ecc` |
| `25[7:0]` | write mask | same mask, `+1ed0` |
| `26[18:16]` | CCW function | `CCW_STENCILFUNC - 1`, `+1ec0` |
| `26[8:6]` | CCW fail | `CCW_STENCILFAIL - 1`, `+1eb8` |
| `26[5:3]` | CCW Z-fail | `CCW_STENCILZFAIL - 1`, `+1ebc` |
| `26[2:0]` | CCW pass | `CCW_STENCILPASS - 1`, `+1ec4` |

The three-bit stencil-op subtraction maps D3D `KEEP, ZERO, REPLACE, INCRSAT,
DECRSAT, INVERT, INCR, DECR` directly to hardware `0..7`.  With two-sided
stencil disabled the driver mirrors the clockwise values into the second face;
with it enabled it may exchange the two packed faces to account for the active
front-face winding.  This is the reason a new driver should derive CW/CCW after
rasterizer orientation rather than permanently equate CW with GL front.

### Alpha test and blending

The alpha-test builder at `VTGUModeDX32A:0x101fb090` has an exact OpenGL copy
at `0x1024edc0`.  It writes type-01 subaddress `33`: `ALPHAFUNC-1` goes to
`[14:12]`, while an 8-bit `ALPHAREF` below 255 is expanded as
`(ref << 2) | (ref >> 6)` into `[10:0]`; the special value 255 becomes `0x400`
exactly.  This matches the manual's fixed-point alpha-reference and eight
comparison modes.

The complete D3D blend builder begins at `0x101fd9e0`; its OpenGL counterpart
begins at `0x10251220`.  Hardware equation words and fields are:

| word | fields | equation role |
|---:|---|---|
| `34` | no-clamp bit 16, RGB operand A `[15:10]`, factor A `[9:4]` | first RGB term |
| `35` | RGB op `[15:14]`, operand B `[13:8]`, factor B `[7:2]` | second RGB term/operator |
| `36` | no-clamp bit 16, alpha operand A `[15:10]`, factor A `[9:4]` | first alpha term |
| `37` | alpha op `[15:14]`, operand B `[13:8]`, factor B `[7:2]` | second alpha term/operator |

The hardware op values are `0=add`, `1=subtract`, `2=max`, `3=min`.  Therefore
the public mapping is exact: `ADD -> 0`, `SUBTRACT -> 1` with source as A,
`REVSUBTRACT -> 1` with destination as A, `MIN -> 3`, and `MAX -> 2`.
`SEPARATEALPHABLENDENABLE` selects `SRCBLENDALPHA`, `DESTBLENDALPHA`, and
`BLENDOPALPHA` for words `36/37`; otherwise the RGB states also drive alpha.
`ALPHABLENDENABLE` sets type-01 subaddress `00[7:4]`; disabling it clears those
four RT blend-enable bits.

The manual calls A34/A36 bit 16 saturation control but defines its polarity as
`0 = clamp to [0,1]`, `1 = do not clamp`.  Both proprietary builders set it;
the live Xorg-derived source-over packet leaves it clear and is clamped.  Exact
common equation words and this distinction are tabulated in
[blend-alpha-packet-recipes.md](blend-alpha-packet-recipes.md).

For the add/subtract source-A ordering, the proprietary factor tables are the
following.  Each table value is already the complete six-bit hardware field;
put RGB A/alpha A at bit 4 and RGB B/alpha B at bit 2.  The OpenGL tables at
`0x10399170..0x10399260` are byte-for-byte equal to the D3D tables at
`0x10336480..0x10336570`.

| D3DBLEND | RGB A | RGB B | alpha A | alpha B |
|---|---:|---:|---:|---:|
| `ZERO` (1) | `05` | `05` | `09` | `09` |
| `ONE` (2) | `15` | `15` | `19` | `19` |
| `SRCCOLOR` (3) | `00` | `00` | `01` | `01` |
| `INVSRCCOLOR` (4) | `10` | `10` | `11` | `11` |
| `SRCALPHA` (5) | `02` | `02` | `01` | `01` |
| `INVSRCALPHA` (6) | `12` | `12` | `11` | `11` |
| `DESTALPHA` (7) | `03` | `03` | `02` | `02` |
| `INVDESTALPHA` (8) | `13` | `13` | `12` | `12` |
| `DESTCOLOR` (9) | `01` | `01` | `02` | `02` |
| `INVDESTCOLOR` (10) | `11` | `11` | `12` | `12` |
| `SRCALPHASAT` (11) | `08` | `08` | `19` | `19` |
| `BOTHSRCALPHA` (12) | `02` | `12` | `01` | `11` |
| `BOTHINVSRCALPHA` (13) | `12` | `02` | `11` | `01` |
| `BLENDFACTOR` (14) | `05` | `05` | `09` | `09` |
| `INVBLENDFACTOR` (15) | `15` | `15` | `19` | `19` |
| `SRCCOLOR2` (16) | `05` | `05` | `09` | `09` |
| `INVSRCCOLOR2` (17) | `15` | `15` | `19` | `19` |

The apparently counterintuitive constant-factor rows are not standalone.
Helper `0x101f9bd0` expands D3DRS `BLENDFACTOR` ARGB8 to four floats and
`0x1020aa50` converts each positive float to the hardware fixed range
`0..0x400`, saturating one and larger values to `0x400`.  For an A-term RGB
constant the builder writes R to `39[22:12]`, G/B to `3a[22:12]/[10:0]`, and
A to `3e[10:0]`; for a B-term constant it uses `3c[22:12]`, both halves of
`3d`, and `3f[10:0]`.  When the alpha equation needs an operand constant,
`3e[22:12]` is Aa and `3f[22:12]` is Ab.  This exactly matches the manual's
constant-register definitions.  `BLENDFACTOR` and the dual-source/private
cases must therefore not be implemented by copying only the six-bit selector.

Reverse-subtract uses the second four lookup tables at D3D
`0x103365c0..0x103366b0` and OpenGL `0x103992b0..0x103993a0`; they are the
same values with A/B roles exchanged, and once again both DLLs agree byte for
byte.  The smallest safe first blend support is consequently ADD with `ZERO`,
`ONE`, source alpha, inverse source alpha, destination alpha, and inverse
destination alpha.  Constant color/alpha, dual-source blending, MIN/MAX, and
separate alpha should remain capability-gated until their extra constant words
are emitted and tested.

Packet-exact guarded D16/LESS and SRC_ALPHA/INV_SRC_ALPHA successors,
including typed SF relocation slots, CPU clear values, and expected samples,
are specified in [depth-blend-oracles.md](depth-blend-oracles.md).

Exact channel-mask remapping, the complete logic-op truth-table conversion,
alpha-test enable policy, fog fields, cube/volume addressing, and presentation
ordering are consolidated in
[fixed-function-texture-sync.md](fixed-function-texture-sync.md).

The exact 232-dword linear one-texture successor is now decoded in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  Its stage words are
`0007fc00,18001100,20020020,21000001,22008055,30980000`; its sampler words are
`31100000,32000052`.  The important corrections are that the source base uses
256-byte units, canonical 32-byte linear pitch is derived from logical width
while only a noncanonical pitch is represented as an effective texel width,
and `32000052` means R clamp plus T/S repeat.  The
apparently fixed `0x40` is the R-axis clamp field, not a sampler enable.

Windows independently confirms every field.  The D3D image builder at
`0x101d7c10` and the copy in the OpenGL DLL at `0x10240740` build subaddresses
`00,18,20,21,22,30` in a `0xc4`-byte per-stage record, including dimensions,
power-of-two/log2 fields, format byte, mip addresses, and layout selectors.
The D3D/OpenGL address builders at `0x101d9250/0x10241a70` map WRAP, MIRROR,
CLAMP, BORDER, and MIRRORONCE to hardware codes `2,3,1,0,7`.  Their filter
builders at `0x101d9380/0x10241ba0` map nearest, linear, anisotropic, and two
4x4 kernels to codes `0,1,2,3,3` in the S/T/R fields.  Both OpenGL routines
are referenced by its dispatch tables, making this shared live hardware-state
logic rather than an unreferenced duplicate.  Tiled high-address selectors,
cube-face allocation order, and cache lifetime remain unresolved.

Exact cull/winding deltas, the distinction between edge AA and unsupported
MSAA, viewport split-word packing, depth-bias fields, and the recovered H6
vertex-stream plus inline-index path are consolidated in
[raster-front-end-oracles.md](raster-front-end-oracles.md).

The D3D query entry points are:

| operation | wrapper | implementation |
|---|---:|---:|
| CreateQuery | `0x100935d0` | `0x100b8640` |
| DestroyQuery | `0x10093600` | `0x100b8690` |
| IssueQuery | `0x10093630` | `0x100b86c0` |
| GetQueryData | `0x10093660` | `0x100b8730` |

The implementations route through `0x100d9b10`, `0x100d9e80`, and
`0x100d9cb0`.  Creation accepts exactly query type 8 or 9; these are the D3D9
`EVENT` and `OCCLUSION` enumerants.  Every other type returns
`0x80070057`.  The query object is 76 bytes and stores the D3D type at `+0x24`,
readiness/state at `+0x10/+0x14`, a four-byte result size at `+0x18`, issue
flags at `+0x1c`, hardware allocations at `+0x28/+0x2c/+0x30`, a context
sequence at `+0x34`, result bookkeeping at `+0x38`, and an event/counter value
at `+0x44`.

Issue decodes the D3D flags directly: bit 1 is BEGIN and bit 0 is END.  The
EVENT branch accepts END and calls the chip callback table's flush/control
entry `+0x28`, followed by its query entry `+0x40`.  The OCCLUSION branch uses
BEGIN/END state, detects a begin/end pair with no intervening context sequence
change as an immediate zero result, and otherwise allocates records through
callbacks `+0x38`, emits through `+0x50`, and completes/reads through
`+0x40/+0x44/+0x5c`.  Issue temporarily changes manager field `+0x2b4` while
the top-level DDI callback runs.  The exact chip-selected packet words and CPU
cache visibility protocol remain unresolved because these calls are indirect
through the per-chip table.

## Remaining draw-path gaps

- Cross-check the now-recovered Linux H6 inline 16/32-bit index packing against
  every Windows D3D draw variant and recover instancing/base-vertex limits.
- Map the remaining D3D layout classes through construction to exact type-01
  address/tile bits, and map private sampler formats through the separate
  type-02 texture lookup.
- Execute the packet-exact ordinary depth/stencil clear and render-to-texture
  cache discriminators in
  [resource-layout-clear-cache.md](resource-layout-clear-cache.md); normal
  vendor clears are now proved to funnel through `sfmBlt`, while fast-Z and
  read-color/read-Z cache pulse ordering remain unresolved.
- Recover query allocation, producer packet, fence ordering, and CPU cache
  visibility.
- Map the remaining tiled RT/resource-layout classes and relocation selector
  16 before exposing modifiers.  Linear pitch units and the conservative
  16352-byte vendor cap are now resolved in
  [resource-layout-clear-cache.md](resource-layout-clear-cache.md).
