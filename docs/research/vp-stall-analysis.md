# VX900 vertex-processor stall: root cause and corrected draw oracle

## Result

The 160-dword Xorg-derived draw used in the first guarded test is not the
VX900 path.  The offline harness initialized the function table with
`viaInit3DState_H6`, but left the chip-generation flag at state offset `0x178`
zero.  This made `via3DEmitState_H5` take its older H5 FVF branch and omit the
VX900 vertex constants.  Feeding vertices after that incomplete state is the
measured cause of `0400=00080031`: command regulator, vertex processor, and
overall 3D busy, with every downstream unit idle.  Replacing that body with the
exact corrected 188 dwords has now completed and rasterized on the live VX900.

The real PCI `1106:7122` Xorg call chain sets this flag before any state is
emitted:

```text
viaExa3DInit+0x98   279f8: mov dword [esi+0x7ac], 1
                    27a02: add esi, 0x634
                    27a0b: call viaInit3DState_H6

0x7ac - 0x634 = 0x178
```

Thus `state+0x178=1` is not optional initialization and is not established by
`viaInit3DState_H6` itself.  It is the Xorg screen object's chip-generation
flag.  The harness's previous `0xcafe` branch missed it.  Its `0xbeef` branch
already set it, which is why that aggregate one-texture capture contained H6
state that the alleged normal zero-texture capture lacked.

Re-running the exact setters and emitter order with the single correction

```text
/tmp/via-xorg-probe via_drv.so 0xcafe 0x178 1
```

changes the body from 160 to **188 dwords**.  No clocks, MMIO, Wine, or guessed
fields are involved in this result: the words below are emitted by the vendor
binary under `qemu-i386`.

## What the missing flag changes

There are four material differences.

1. The general texture block gains `05000000` before `0D810000`.
2. The entire type-04 FVF/front-end prefix changes from the old three words
   `00000006 01020100 02060503` to the VX900 words below.
3. The aggregate emitter adds the H6 vertex floating-constant packet for
   constant register 256 and the following register: `(1,1,1,1)` and
   `(786432,786432,0,0)`.
4. It adds the H6 vertex integer-register packet containing four zeros.

The corrected FVF begins:

```text
00000206 010000C2 020041D0 030052D0
04006342 1200001F 61800000 622081FF
```

The public VX900 manual proves the non-reserved part of this mapping:

- subaddress `00`: `HVBDNum=2`, hence D0 through D2 are active, and total
  vertex input length is six dwords;
- subaddress `01`, value `0000c2`: D0 at dword offset 0, input register 0,
  4D 32-bit float (`x,y,z,w`);
- subaddress `02`, value `0041d0`: D1 at offset 4, input register 1, four-byte
  color (diffuse) with normalization **disabled**;
- subaddress `03`, value `0052d0`: D2 at offset 5, input register 2, four-byte
  color (specular) with normalization **disabled**;
- subaddress `04`, value `006342`: the prepared two-float texture-coordinate
  mapping at offset 6/IR3; it is inactive when `HVBDNum=2` and becomes active
  for a textured vertex;
- subaddress `12`, value `00001f`: input-register fill mask used by this path.

Subaddresses `61` and `62` are marked reserved in the public manual but are
unconditionally emitted by the H6 branch.  Their values depend on active
texture count (`62` contains `(texture_count+2)<<20` after masking), so they are
front-end/VS resource-linkage controls and must be copied exactly.  They are
not safe to omit merely because the PDF calls them reserved.

The old 160-dword branch instead describes an older FVF encoding.  Decoding it
as VX900 state leaves no valid D0 mapping under the public H6 layout.  This is
precisely the kind of mismatch that lets shader RAM stores and control writes
complete while the VP waits forever once vertex data arrives.

The low format bit is independently important.  The manual defines D3DCOLOR
as `d1`, not `d0`: bit 0 enables division by 255.  The historical Xorg packet
uses `d0`, so byte values enter the VS as 0..255 floats.  Its opaque 00/ff
colors hid that distinction.  A live blend sweep later proved the effect:
vertex alpha 0 stayed transparent, while 40/7f/80 all saturated to opaque.
General packed API colors must change A02/A03 to `020041d1/030052d1`.

## Exact corrected 188-dword body

This excludes the outer `006c=00800000` 2D/3D synchronization packets and CR
tail/padding, exactly as the previous 160-dword body did.  The only
target-specific word is `5007FD00` (`target_byte_address >> 8`).

```text
# destination, blend and common state: dwords 0..23
FE020440 0000043C 00010000 CC000000
5007FD00 51000000 52980004 38000000
3B000000 39000000 3C000000 34000150
35000114 36000500 37000200 860000FF
870000FF 53010C0F 00000011 01C01540
88001ED0 89000800 CC000000 CC000000

# general texture/attribute control: dwords 24..35
FE020440 0000043C FE020000 CC000000
00000001 01002521 08010100 05000000
0D810000 CC000000 CC000000 CC000000

# VX900 FVF, primitive and viewport linkage: dwords 36..67
FE020440 0000043C 00040000 CC000000
00000206 010000C2 020041D0 030052D0
04006342 1200001F 61800000 622081FF
201C5C00 23000F02 24000004 25F6CC00
28FFCC00 40800000 41400000 4200493F
43800000 44400000 4500493F 46800000
47400000 4800003F 49000000 50000000
51000000 52147FFF CC000000 CC000000

# existing PS constant store: dwords 68..75
FE020440 0000043C 22030000 CC000000
00000000 00000000 00000000 00000000

# H6 VS float constants, exact H3 packet: dwords 76..87
FE03043C 00000008 32030400 CC000000
3F800000 3F800000 3F800000 3F800000
49400000 49400000 00000000 00000000

# H6 VS integer constants: dwords 88..95
FE020440 0000043C 33030000 CC000000
00000000 00000000 00000000 00000000

# clip: dwords 96..103
FE020440 0000043C 00010000 CC000000
80000020 81000020 82000020 83000020

# exact four-instruction H6 vertex program: dwords 104..123
FE020440 0000043C 31030000 CC000000
00380150 00000000 00000000 0008010F
0008014C 00100150 00384154 0004021F
0008414C 00000000 00000000 0008422F
0008814C 00000000 00000000 0008423F

# VS-valid/last-instruction control: dwords 124..131
FE020440 0000043C 00040000 CC000000
60800003 CC000000 CC000000 CC000000

# no-texture pixel controls: dwords 132..151
FE020440 0000043C 00010000 CC000000
90058000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 CCCCCCCC
DDDDDDDD CC000000 CC000000 CC000000

# pixel program: dwords 152..159
FE020440 0000043C 20030000 CC000000
01C00000 301C0000 00F04005 000D083F

# TL, TR, BR, BL immediate vertices: dwords 160..187
FE020440 0000043C 00000000 CC000000
BF000000 BF000000 00000000 3F800000 FFFF0000 FFFF0000
41FC0000 BF000000 00000000 3F800000 FFFF0000 FFFF0000
41FC0000 41FC0000 00000000 3F800000 FFFF0000 FFFF0000
BF000000 41FC0000 00000000 3F800000 FFFF0000 FFFF0000
```

The second instruction's third dword is definitively `00384154`.  The binary
sets byte `state+0x172` to `0x38` at `0x30c85..0x30c99`, and the offline output
above independently confirms the resulting word.  `00004154` is not the
output of this vendor path.

## Measured live result

The exact 188-dword body above was submitted after the correct 2D-to-3D engine
initialization and the proven cache-flushed, two-phase CR prime.  It succeeded:

```text
pause=0x4f0
CR=0xc0000000
engine=0x00000000
error=0x00000000
pixels=1024/1024 at 0x000000ff
guards=intact
restored=yes
```

Thus the command reader reached PAUSE, every 3D busy bit cleared, no engine
error was raised, every expected target pixel changed to the shader result,
neither poison guard changed, and the saved target span was restored.  The
persistent tested module is
`/root/tmp/chrome9/chrome9-h6-draw-verified.ko`, SHA-256
`71290715146fa8a0565870d422e12d1e14d5e6366821f3553835b37714eb86b9`.
This is direct silicon proof of the complete packet sequence, not merely proof
that its state stores are accepted.

The observed `000000ff` is also now decoded exactly; it is not evidence of an
ARGB/ABGR ambiguity.  Destination word `52980004` is linear ARGB0888
(`x8r8g8b8`) with 128-byte pitch, so logical blue packs as `000000ff` and
logical red as `00ff0000`.  Word `53010c0f` selects COPY_PEN and enables all
four channel writes.  Although each vertex contains diffuse `ffff0000`, type-04
word `23000f02` selects solid shading (`bits 14:12 = 000`), and type-01 words
`860000ff 870000ff` set that solid color to R=0, G=0, B=255, A=255.  The pixel
program copies its full input color to `oC0`.  The resulting framebuffer value
must therefore be `000000ff`.  The complete manual, Linux, VS, and PS evidence
is in [windows-draw-path.md](windows-draw-path.md).

## Ranked prerequisite analysis

1. **Missing `state+0x178=1` H6 branch -- proved root cause.**  It changes the
   exact front-end mapping and supplies constants used by the H6 VS.  The old
   160-dword vector stalled at the VP; the otherwise matched corrected
   188-dword draw retired idle and rasterized all 1,024 pixels.
2. **Missing H6 VS constants -- proved consequence of item 1.**  The PDF notes
   that C-to-S scaling/offset must be placed in both clipping state and VS
   constants c256/c257.  The old vector had only clipping state `40..52`; the
   corrected vector also uploads the H6 constants.
3. **Missing hidden H6 resource controls `61/62` -- proved consequence of item
   1.**  These reserved-in-PDF registers are emitted by the actual 7122 branch
   and scale with texture count.
4. **3D clocks or global engine initialization -- excluded as the remaining
   cause of this failure.**  Those conditions were held constant between the
   stalled 160-dword and successful 188-dword runs.
5. **Packet ordering, shader-valid word, or PS setup -- excluded as the
   remaining cause.**  Those words and their order were unchanged; replacing
   the mixed H5/H6 state was sufficient for a successful raster draw.

## Safest live discriminator

Do not patch individual missing words into the old body.  Replace it wholesale
with the 188-dword oracle, because the old and new FVF prefixes are mutually
exclusive branches.

The smallest address-free discriminator is:

1. emit corrected blocks through the VS integer constants (dwords 24..95),
2. upload the same VS program and `60800003`,
3. emit no type-00 vertex data,
4. finish with the already proven status packet and guarded 2D regression.

This should idle and proves every newly found store/control packet is accepted.
It cannot prove FVF consumption.  The full corrected 188-dword draw has since
passed with the existing 4 KiB poison pages, exact VRAM backup/restore, bounded
pause/idle waits, and reboot recovery.  These state-only sequences remain
useful as focused regression tests.  A partial one-vertex test remains unsafe
and unnecessary:
by design the VP can wait for the remaining vertex fields/list completion, so
its busy result would be ambiguous and may itself require reset.

### Exact address-free pre-vertex probe

The smallest packet-shape-supported H6-delta probe is **72 dwords**.  It has no
destination, texture base, vertex-buffer base, or writeback address; type-00
vertex data is absent, so no primitive can launch.  It retains the complete
12-dword general packet because that exact packet came from Xorg, but trims the
type-04 packet to the eight H6 declaration/linkage words that differ from the
rejected H5 branch.  This shortening is not a packet-format guess: the Windows
native VS emitter at `VTGOGL32A.dll:0x102055e3..0x10205671` emits a type-04
packet containing only three register words and pads it to a 16-byte boundary.
Emit these packets in this order:

```text
# 12 dwords: exact corrected general texture/attribute packet
FE020440 0000043C FE020000 CC000000
00000001 01002521 08010100 05000000
0D810000 CC000000 CC000000 CC000000

# 12 dwords: H6 immediate-FVF mapping and VS linkage only
FE020440 0000043C 00040000 CC000000
00000206 010000C2 020041D0 030052D0
04006342 1200001F 61800000 622081FF

# 12 dwords: H6 float constants c256/c257
FE03043C 00000008 32030400 CC000000
3F800000 3F800000 3F800000 3F800000
49400000 49400000 00000000 00000000

# 8 dwords: H6 integer constants
FE020440 0000043C 33030000 CC000000
00000000 00000000 00000000 00000000

# 20 dwords: known vendor VS
FE020440 0000043C 31030000 CC000000
00380150 00000000 00000000 0008010F
0008014C 00100150 00384154 0004021F
0008414C 00000000 00000000 0008422F
0008814C 00000000 00000000 0008423F

# 8 dwords: VS valid plus last instruction index
FE020440 0000043C 00040000 CC000000
60800003 CC000000 CC000000 CC000000
```

For the first live run, split this at the activation boundary:

1. Submit only the first four packets, **44 dwords** through the integer
   constants.  This tests every newly recovered declaration/linkage/constant
   write without uploading or validating an executable program.  It must
   retire idle.
2. After a fresh proven prime, submit the complete **72-dword** sequence.  The
   already tested program store is repeated immediately before `60800003`, so
   the valid/last-instruction control never refers to stale instruction RAM.

The conservative fallback is the former **92-dword** sequence: keep the exact
12-dword general packet and replace the shortened 12-dword type-04 packet with
the full 32-dword Xorg block at corrected-body dwords 36..67.  That writes 20
additional, unchanged primitive/viewport registers but is otherwise identical.
It is appropriate if the implementation deliberately avoids synthesizing a
short type-04 transaction.  The 72-dword form is the smallest recommended
probe; an isolated five-word general packet was not observed in either vendor
driver and is therefore not recommended merely to save four padding dwords.

As with the existing store probes, place the proven H0
`006c=00800000` synchronization packet before the probe and the proven
eight-dword `hwmUpdateStatus_h5` packet after them:

```text
FE020440 0000043C 00010000 CC000000
1C000000 1D000000 CCCCCCCC CCCCCCCC
```

The state-only pass criteria are:

- the CR reaches its programmed pause and `0444.bit31` becomes one;
- all meaningful busy bits in `0400` clear, especially bit 5 (VP), bit 4
  (CR), and bit 0 (overall 3D); on the current healthy baseline this is
  expected to read `00000000`;
- `040c` remains unchanged (`00000000` on the healthy baseline);
- the existing guarded 2D regression still changes all 1024 expected pixels
  and neither guard changes;
- `043c` is recorded diagnostically but is not used as retirement evidence,
  because the request FIFO may fetch ahead.

If the 44-dword first stage leaves VP busy, declaration/linkage/constants alone
are rejected and the 72-dword stage must not run.  If 44 retires but 72 leaves
VP busy, `60800003` activation (or its relationship to instruction RAM) is the
new exact boundary.  If both idle, the result proves state acceptance and
narrows the next guarded test to FVF consumption by type-00 vertices.  Neither
stage proves rasterization or shader outputs.

An audit script compared the 188 hexadecimal words in this document against a
fresh dynamic vendor emission and reported `actual 188 doc 188 equal True`.
The dynamic emission was generated from the hashed `via_drv.so` through the
same qemu-i386 harness, with only `state+0x178=1` added.

The audit was repeated after deriving the shortened probe.  A token-for-token
`diff` between a fresh execution and this document was empty; each side had
exactly 188 words.  The executed binary's SHA-256 was
`5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787`.
Static disassembly supplies a second, independent provenance chain for every
new H6 word:

| binary range | proved effect |
|---|---|
| `0x2f2c5..0x2f2cd`, target `0x2f888` | test `state+0x178`; insert `05000000` |
| `0x2f376..0x2f41e` | choose H6 declaration branch; construct `00..04`, `12`, `61`, `62` |
| `0x2f64a..0x2f71e` | gate on the same flag; emit subtype-32 selector, count 8, and eight float-constant dwords |
| `0x2f739..0x2f7d5` | emit subtype-33 and four zero integer-constant dwords |
| `0x30b40..0x3113b` | emit subtype-31 VS program, then form `60800000 | (texture_count + 3)` |

The exact zero-texture arithmetic at `0x2f384..0x2f415` is
`HVBDNum=(0+2)`, length `2*0+6`, and subaddress 62 resource count
`(0+2)<<20`, yielding `00000206` and `622081ff`.  Thus the critical prefix is
not only a dynamic capture: all of its variable fields reproduce directly from
the normal-path state and the static instructions.

### Independent Windows-driver confirmation

Both Windows user-mode drivers contain the same native VS-state emitter:

| binary | emitter region |
|---|---:|
| `VTGOGL32A.dll` | `0x102052e0..0x10205714` |
| `VTGUModeDX32A.dll` | `0x1015ad00..0x1015aff4` |

They maintain three consecutive type-04 control words at context offsets
`0x2260`, `0x2264`, and `0x2268`.  The code proves these masked fields:

- `+0x2260`/subaddress `60`: bit 23 set, low nine bits are native instruction
  count minus one;
- `+0x2264`/subaddress `61`: bit 23 is set when the compiler object's first
  resource count is at most 8; bits 22:20 come from object `+0x08`, and bit 19
  records whether object `+0x0c` is nonzero;
- `+0x2268`/subaddress `62`: low nine bits come from object `+0x10`.

The Windows emitter then writes an H2/type-04 packet containing `61`, `62`, and
`60`, followed by an H3 subtype-31 VS-program packet.  Before those it emits an
H3 subtype-32 constant packet beginning with the same selector `32030400`.
The Windows path copies 48 constant bytes for its general compiled shader,
whereas this Xorg rectangle shader intentionally uploads only the eight dwords
shown above.  The payload sizes therefore must not be conflated.

This is independent confirmation that `61/62` are live VS resource/linkage
registers despite being marked reserved in the public PDF, and that all three
`60/61/62` controls belong to one coherent VS setup.  The rejected 160-dword
stream carried only `60`; the corrected Xorg stream carries all three.

## Adjacent facts recovered while auditing the stall

- In the VX900 type-04 schema, `00000206` directly proves a six-dword immediate
  vertex comprised of one float4 and two packed colors.
- `04006342` is the next two-float mapping used when a texture is active.
- A normal one-texture aggregate capture uses eight input dwords and activates
  the texture mapping; the corresponding H6 controls change with texture count.
- For active texture count `n` in the Xorg rectangle path, the H6 emitter forms
  subaddress `00` as `((n + 2) << 8) | (2*n + 6)`.  Consequently the exact
  input lengths are six, eight, and ten dwords for zero, one, and two textures,
  with `HVBDNum` selecting D0..D2, D0..D3, and D0..D4 respectively.  The
  second texture adds `05008442` (offset 8, IR4, 2D float).
- Hidden subaddress `62` is `620081ff | ((n + 2) << 20)` after the vendor
  mask: `622081ff`, `623081ff`, and `624081ff` for zero through two textures.
  Subaddress `61` remains `61800000` in this path.
- The public PDF lists vertex shader instruction/float-constant/integer uploads
  as palette subtypes `31/32/33`, but later calls `31..33` reserved.  The binary
  resolves that documentation contradiction: all three are live vendor-used
  packet classes on 7122.
- The H3 float-constant packet's second dword is a payload count (`8` here),
  while `32030400` selects the VS constant class/start.  It must not be encoded
  as a generic H2 packet.
