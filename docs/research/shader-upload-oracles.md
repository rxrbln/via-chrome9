# Chrome9 HD shader upload, linkage, and execution oracles

## Result

The programmable upload path is sufficiently decoded to run two conservative
shader tests without inventing packet fields:

1. the complete 188-dword Xorg no-texture draw is a silicon-proven
   VS-to-PS varying pass-through;
2. changing only six dwords in that proven body gives a constant-color pixel
   shader test while retaining every proven H6 front-end and activation word;
3. the complete 232-dword one-texture draw is already a silicon-proven
   texture-sample shader test in local VRAM, translated contiguous DMA memory,
   and cached shmem.

The second test is packet-exact but remains a proposed derivative: the native
`MOV oC0,c0` instruction is produced identically by the Linux 5.75 and Windows
2015 compilers, but that six-dword mutation has not yet run on silicon.  It is
safer than replacing the whole pixel control block with an offline compiler's
zero-runtime-state image.

## Packet ABI and activation order

The shader stores use the 3D parameter port at MMIO `0440`, selected through
`043c`.  The selectors and exact payload units are:

| selector | payload element | meaning |
|---:|---:|---|
| `20030000` | four dwords | one pixel ALU instruction |
| `21030000` | one dword | one pixel texture-address instruction |
| `2203xxxx` | one dword | pixel float constants, start dword in low 16 bits |
| `31030000` | four dwords | one vertex instruction |
| `3203xxxx` | one dword | vertex float constants, start dword in low 16 bits |
| `33030000` | one dword | packed vertex integer constants |

H3 uploads have the exact shape:

```text
FE03043C <payload-dword-count> <selector> CC000000
<payload>
```

The Xorg rectangle paths also use aligned H2 streams for the same selector
families.  The silicon-proven oracles retain the packet form emitted by Xorg;
H2 and H3 should not be interchanged merely because their final parameter
selector and payload are the same.

The complete H6 vertex activation sequence is not just subaddress 60.  The
Windows D3D and OpenGL emitters write type-04 words in order `61,62,60` and
then upload the subtype-31 program.  Subtype-32 float constants precede that
group.  In the proven Xorg zero-texture stream the state is distributed in
normal dirty-state order but is equivalent:

```text
# declaration/resource state, earlier in the type-04 group
00000206 010000C2 020041D0 030052D0
04006342 1200001F 61800000 622081FF

# internal float constants c256 and c257
FE03043C 00000008 32030400 CC000000
3F800000 3F800000 3F800000 3F800000
49400000 49400000 00000000 00000000

# packed integer constants
FE020440 0000043C 33030000 CC000000
00000000 00000000 00000000 00000000

# four native VS instructions
FE020440 0000043C 31030000 CC000000
00380150 00000000 00000000 0008010F
0008014C 00100150 00384154 0004021F
0008414C 00000000 00000000 0008422F
0008814C 00000000 00000000 0008423F

# valid plus last native instruction index
FE020440 0000043C 00040000 CC000000
60800003 CC000000 CC000000 CC000000
```

`60[23]` is valid.  For this hand-built Xorg shader `60[8:0]` is native
instruction count minus one.  The general compiler path instead copies
`object+0x14 - 1`; it uploads `object+0x424` records, and offline VS 2.0
differentials show two extra uploaded tail records.  Thus payload length and
active last index must remain separate even though they coincide for the Xorg
and DRI rectangle oracles.
Windows constructs `61[22:20]` and `61[19]` from compiler resource metadata,
and `62[8:0]` from another compiler count.  The Xorg rectangle path also puts
the active front-end resource count in `62[23:20]`: `2`, `3`, and `4` for
zero, one, and two textures.  Therefore `60800003` must never be used as a
standalone generic activation word, and synthetic `00800003` is invalid.

Pixel activation consists of the type-01 `90..9b` control words plus the
type-20 and, when nonempty, type-21 program stores.  The general compiler
update path emits ALU then TAU, while the proven Xorg texture shader emits TAU
then ALU; retain the order of the selected complete oracle.  The
compiled-object control sources are exact:

| state word | compiled-object source |
|---:|---|
| `90` | `object+0x2c OR object+0x6c`; `+2c.bit18` selects ALU first |
| `91` | `object+0x70` |
| `92` | `object+0x18 OR (object+0x24 << 16)` |
| `93`,`94` | `object+0x60,+0x64`; TAU-to-ALU transition bits |
| `95`..`9a` | `object+0x48..+0x5c`; ALU-to-TAU transition bits |
| `9b` | runtime state masked to 24 bits |

Thus `92[15:0]` is ALU count and `92[31:16]` is TAU count.  Transition maps
pack 24 instruction positions per control dword: `A93/A94` cover TAU indices
0..31, while `A95..A9a` are the six-word ALU map (the proprietary compiler
uses indices 0..95).  A set bit switches to the other store after that
instruction.  Offline differentials validate the decode: `A93=1`, `2`, and
`3` mean switch after TAU 0, after TAU 1, and after both TAUs respectively;
`A95=1` switches after ALU 0.  Preserve the finalized scheduling tuple.

The target post-translation configuration is now exact, not an opaque runtime
choice.  The normal Linux creation path calls
`stmPPS_PostTranslationSetting_invi(compiled+0x10, 2)`.  Configuration two
turns compiler metadata `+2c=00040000` into `00058000` for an ALU-only
program, and zero into `00018000` for one TAU followed by ALU.  It also sets
the texture case's `+70=1`, TAU dependency bit 28, and the consuming ALU
source dependency bit 17.  Consequently a complete generic one-texture
MODULATE transaction uses:

```text
A90..A93 = 90018000 91000001 92010001 93000001
ALU       = 0140b6d0 30040053 00f20005 00000820
TAU       = 301001b0
```

The fixed Xorg program uses allocated r1 instead (`301101b0` and the matching
ALU id), but its controls and finalizer-owned bits are identical.  Raw
compiler outputs `00f00005/201001b0` are semantic templates only and must not
be passed to the program stores.

Configuration two is equally material for multiple textures.  It preserves
the independently recovered control maps but assigns per-record TAU high bits
and the consuming ALU dependency field according to the phase schedule:

```text
# texld r0,t0,s0; texld r1,t1,s1; mul oC0,r0,r1
A90..A95  90000000 91000001 92020001 93000002 94000000 95000000
TAU       a01001b0 311001b1
ALU       0140b6d0 3003f000 00f20005 00000820

# texld r0,t0,s0; texld r1,r0,s1; mul oC0,r0,r1
A90..A95  90000000 91000001 92020002 93000003 94000000 95000001
TAU       201001b0 b11001b1
ALU       01c00530 301c0053 00f00005 000d0420
          0140b6d0 3003f000 00f20005 00000820
```

Do not implement finalization as `TAU |= 0x10000000`: the first independent
TAU instead gains bit 31, and the dependent schedule gives the second TAU both
bits 31 and 28.  The compiler/finalizer transaction or an exactly equivalent
dependency analysis must own these words as a unit with A90..A9a.

## Exact VS behavior and varying linkage

The four-instruction Xorg VS above is the known-good pretransformed rectangle
shader, not a generic one-instruction passthrough:

- instruction 0 and instruction 1 perform the position/viewport conversion
  using the uploaded internal constants;
- instruction 2 is `MOV oD0,v1`;
- instruction 3 is `MOV oD1,v2`.

The historical declaration maps immediate input D0 to VS `v0` as float4
position and D1/D2 to `v1/v2` as BGRA color, but its format byte `d0` leaves
normalization disabled.  The manual's normalized D3DCOLOR encoding is `d1`.
Opaque 00/ff oracle colors do not distinguish them; partial color or alpha
requires A02/A03 `020041d1/030052d1`.  The full-parser output mapping is
fixed:

| VS output | unified destination | native MOV control |
|---|---:|---:|
| `oPos` | 32 | `00080a0f` |
| `oD0`,`oD1` | 34,35 | `0008422f`,`0008423f` |
| `oFog`,`oPts` | 38,39 | `0008426f`,`0008027f` |
| `oT0`..`oT3` | 40..43 | `0008028f`..`000802bf` |

The valid continuation is `oT4..oT7` at unified destinations 44..47.  The
vendor frontend fails to range-check beyond it: `oT8` wraps to
`0008020f`, `oT15` to `0008027f`, and a tested `oT16` crashes the offline
compiler.  Enforce eight texture-coordinate outputs in Mesa.

For the no-texture draw, rasterized `oD0` arrives at pixel input class 1,
id 0 (`v0`).  The proven pixel instruction

```text
01C00000 301C0000 00F04005 000D083F
```

is finalized `MOV oC0,v0`, full mask.  The PS destination class is 2, id 0.
This establishes the first VS/PS varying link without guessing any semantic
register numbers.

The one-texture VS adds exactly one instruction:

```text
0008C14C 00000000 00000000 0008028F
```

It is `MOV oT0,v3`; D3 is the two-float texture coordinate declared by
`04006342`.  Texture word `301101B0` samples stage 0 into allocated temporary
1, and the finalized ALU instruction

```text
01C00000 301C0000 00F20105 000D083F
```

is `MOV oC0,r1`.  This is the exact varying-to-TAU-to-PS linkage proven by the
232-dword texture oracle.

## Oracle A: proven varying pass-through

Use the exact 188-dword body in
[vp-stall-analysis.md](vp-stall-analysis.md).  Its shader-relevant dword
ranges are:

| dwords | contents |
|---:|---|
| `36..67` | H6 declaration/resource/primitive/viewport state |
| `68..75` | PS c0 store, four zeros |
| `76..87` | VS c256/c257 store |
| `88..95` | packed VS integer constants |
| `104..123` | four-instruction VS |
| `124..131` | VS valid/last-index word |
| `132..151` | no-texture PS controls |
| `152..159` | `MOV oC0,v0` |

This exact body has completed on VX900 silicon and filled all 1,024 target
pixels.  It proves program upload, activation, position conversion, `oD0` to
PS `v0`, PS output, and render-target writeback together.

## Oracle B: six-dword constant-color derivative

Start from the exact 188-dword body and change only these indexed dwords:

| body dword | old | new | purpose |
|---:|---:|---:|---|
| 72 | `00000000` | `3F800000` | c0.x = 1.0 |
| 75 | `00000000` | `3F800000` | c0.w = 1.0 |
| 156 | `01C00000` | `01C00530` | finalized constant MOV word 0 |
| 157 | `301C0000` | `301C0053` | finalized constant MOV word 1 |
| 158 | `00F04005` | `00F08005` | source A becomes constant c0 |
| 159 | `000D083F` | `000D0820` | finalized `oC0` destination/dependencies |

Using the same uppercase, one-dword-per-line normalization as the source
oracle, the resulting 188-dword body has SHA-256
`db0faae161c571f24d65d401923140e57bfeffc8ba0f86956daeb533bc5d4892`.

The resulting shader portions are exactly:

```text
# c0 = opaque logical red; retain the proven Xorg H2 packet shape
FE020440 0000043C 22030000 CC000000
3F800000 00000000 00000000 3F800000

# retain the proven Xorg no-texture controls verbatim
FE020440 0000043C 00010000 CC000000
90058000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 CCCCCCCC
DDDDDDDD CC000000 CC000000 CC000000

# finalized by both proprietary compilers: MOV oC0,c0
FE020440 0000043C 20030000 CC000000
01C00530 301C0053 00F08005 000D0820
```

Everything else, including the four-instruction VS, `61/62/60`, viewport,
primitive, target, and vertex payload, stays byte-for-byte identical.  With
the proven ARGB0888 target and COPY blend state, all covered pixels should be
`00ff0000`.  This derivative deliberately leaves the Xorg control word
`90058000` as a safe resource superset; it tests constant-store addressing and
the constant-source ALU encoding while changing the fewest live words.

The pre-post-translation zero-runtime-state PS block is separately known:

```text
FE020440 0000043C 00010000 CC000000
90040000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 9B000000
```

Linux 5.75 reports object metadata `+2c=00040000`, ALU count one, TAU count
zero, and all other relevant fields zero before its mandatory target pass;
the Windows backend emits the same native instruction.  Configuration two
changes A90 to `90058000`.  The `90040000` tuple is retained only as a
compiler-phase discriminator and must not be executed.  Packet termination
and alignment in either case must come from the normal H2 builder rather than
copying a naked 12-word array.

## Oracle C: proven texture sample

Use the exact 232-dword body in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  Its programmable
delta from Oracle A is exact:

```text
# declaration/resource changes
00000308 ... 04006342 ... 623081FF

# five-instruction VS, followed by count-minus-one = 4
...the four proven instructions...
0008C14C 00000000 00000000 0008028F
60800004

# PS counts: one TAU, one ALU
90018000 91000001 92010001 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9A000000

# sample stage 0 to allocated r1
301101B0

# MOV oC0,r1
01C00000 301C0000 00F20105 000D083F
```

Do not execute these shader words without the texture image and sampler
blocks in the complete oracle.  The full stream has sampled an asymmetric
red/green/blue/white quadrant image with zero bad pixels and intact guards in
all three tested memory domains.  It is stronger evidence than a uniform
source, which cannot distinguish a real sample from constant output.

## Instruction and constant layout needed by an encoder

Each VS instruction is four dwords.  Its three source words use bit 0 negate,
bits `4:2,7:5,10:8,13:11` for W,Z,Y,X three-bit selectors, and bits `22:14`
for a unified nine-bit source index.  Selector values 0..3 mean X/Y/Z/W, 4
means literal one, and 5 means literal zero; 6/7 remain unassigned.  VS input,
temporary, float-constant, integer, and bool
bases are 32, 64, 256, 96, and 112.  Destination/control dword bits `3:0` are
the mask, `9:4` the unified destination, bit 14 saturation, bits `21:17` the
primary operation, and bits `27:24` the shared relative-address selector.
Only source negate is safe to encode today; other VS source modifiers remain
unsupported.  The vendor frontend accepts all source-modifier nibbles but
reduces them to `modifier & 1`: odd values set the operand's negate bit and
even values become NONE.  Destination modifiers are reduced the same way to
dword-3 saturate bit 14.  A Mesa validator must therefore allow only
NONE/NEG on sources and NONE/SAT on destinations instead of inheriting these
silent aliases.

VS temporary destinations have only a four-bit native id and the vendor
emitter masks rather than validates it (`r16` aliases `r0`).  Keep the initial
allocator within the VS 2.0 `r0..r11` budget.

The vendor arithmetic lowerings use the literal selectors directly:
`00206490` has four selector-4 fields and means one, while `00206db4` has four
selector-5 fields and means zero.  Their unified-id field happens to contain
129 but is ignored by the literal selection.  Preserve those exact words for
the ADD/MUL MAD lowerings; they are not user constants.

Each PS ALU instruction is four dwords.  The independently reproduced fields
include all three source ids/classes/swizzles/modifiers, destination id/class,
mask, saturation, result scale, and the main sub-operation.  The finalizer also
sets dependency bits and low dword-3 slots; never rebuild or zero those after
scheduling.  The texture-address instruction is one dword; TEXLD/TEXLDB/
TEXLDP operation values are 1/3/7 in bits `23:20`, and sampler id appears in
both bits `27:24` and `3:0`.  Destination/source fields overlap in bits 17:16,
so TAU words must be formed after register allocation.

PS ALU swizzles use the same three-bit selector values.  Source B packs XYZW
at dword-0 `15:13,12:10,9:7,6:4`; source C packs XYZW at dword-1
`11:9,8:6,5:3,2:0`; source A packs W at dword-1 `30:28`, Z across dword-1
bit 31 and dword-2 `1:0`, then Y/X at dword-2 `4:2,7:5`.

Constant storage is:

| stage | API-visible store | exact upload mapping |
|---|---|---|
| VS float | `c0..c255` | 256 vec4; selector `3203xxxx`, four dwords per vector |
| VS integer | `i0..i15` | 16 dwords; each int4 is four low bytes packed XYZW; H3 selector `33030000 | (first >> 2)` |
| VS bool | `b0..b15` | unified source ids 112..127; one bit each in type-04 A61[15:0] |
| PS float | `c0..c15` | native ids 0..15 through `22030000` |
| PS float | `c16..c31` | native ids 32..47 through `22030080` |
| PS internal | 7 vec4 per bank | native ids 16..22 and 48..54 through `22030040/c0` |

PS 2.0 constants are uploaded as IEEE-754 dwords.  The legacy PS 1.x path's
clamped C1 conversion must not be applied to PS 2.0.

The 32-constant limit is not self-validating.  Invalid `c32..c47` map to
native ids 48..63 (overlapping the second internal/gap bank), and invalid
`c48..c63` wrap to native ids 0..15.  Reject indices above 31.

The VS nine-bit unified source index similarly makes invalid `c256` wrap from
computed index 512 to unified register zero, not to a constant.  Enforce the
`c0..c255` limit before native mapping.

Range checks must precede VS native register mapping.  The vendor emitter
keeps integer/Boolean operands in native class 3 and masks their low id to
five bits, so invalid `i16` aliases `b0`, invalid `b16` aliases `i0`, and ids
32 wrap again.  No corresponding storage exists: the integer upload has only
16 dwords and A61 only 16 Boolean bits.

The 2015 Windows D3D updater gives a direct packet oracle for a complete VS
integer store (`VTGUModeDX32A.dll:0x10159413..0x1015948c`):

```text
FE03043C 00000010 33030000 CC000000
<16 packed XYZW-byte dwords>
```

Its immediately following Boolean packer (`0x101594aa..0x10159500`) updates
only saved A61[15:0].  Both Linux and Windows shift the stored Boolean byte
without masking it, so Mesa must canonicalize every `bN` to 0 or 1 before
building the A61 mask.

## Generic VS output linkage, bit for bit

The hand-written Xorg shader deliberately leaves type-04 A62's low nine bits
at `1ff`.  That is a valid rectangle oracle but it is **not** the value used by
the generic compiler path.  Linux `stmUpdateVS_inv:0x17c5b9..0x17c5d1` masks
native-object `+0x10` into saved `A62[8:0]`.  The 2015 Windows updater performs
the same masked replacement at
`VTGUModeDX32A.dll:0x1015aca0..0x1015acb7`, then emits saved words in the exact
order A61, A62, A60 at `0x1015aeff..0x1015af27`.

Controlled, hardware-free Linux compiler differentials initially looked like
an output-vector count.  For simple full-MOV programs the results are
independent of which class contributes the vectors:

| complete outputs | object `+4` | object `+0x10` |
|---|---:|---:|
| oPos only | `00000004` | `0` |
| oPos + oD0 | `00000044` | `1` |
| oPos + oD0 + oD1 | `000000c4` | `2` |
| oPos + oT0.xy | `00000104` | `1` |
| oPos + oT0.xy + oT1.xy | `00000204` | `2` |
| oPos + oD0 + oT0.xy | `00000144` | `2` |

The last case returns eight uploads and metadata
`+0,+4,+8,+c,+10,+14 = 1,00000144,0,0,2,6`.  Its three user records are
followed by three active finalizer records and two inactive uploaded sentinels.
Therefore a generic oPos/oD0/oT0 program must use the initialized A62 control
word with low nine bits replaced by two; copying `...81ff` from the fixed
rectangle path is not compiler-equivalent.

M4x4/partial-position differentials prove that “output count” is too broad a
name.  A four-row M4x4 into r0 followed by full oPos MOV has `+10=3` despite
having no non-position output.  Adding T0.xy changes it to five; adding both
T0/T1.xy changes it to six.  M4x4 plus oD0 and T0.xy also gives six.  Thus
`+0x10` is an opaque compiler-derived A62 resource value influenced by
partial writes/position temporaries as well as output linkage.  Preserve it
from the selected compiled template; do not recompute it from `outputs_written`.

`stmUpdateFVFOut_H5i:0x1ba550..0x1baa87` converts object `+4` into the
corresponding type-04 output format.  With point sprites disabled, the exact
construction is:

```text
A25.low24 = 00f00000
            | (oPts ? 00080000 : 0)
            | (oD0  ? 00040000 : 0)
            | (oD1  ? 00020000 : 0)
            | (oFog ? 00010000 : 0)
            | tc0_format << 12 | tc1_format << 8
            | tc2_format << 4  | tc3_format

A26.low16 = tc4_format << 12 | tc5_format << 8
            | tc6_format << 4 | tc7_format

dimension metadata 3,0,1,2 -> format nibble 8,c,e,f
                               # 1D,2D,3D,4D
A28[18:17] = A25[18:17]        # every other A28 bit is preserved
```

The routine takes the active texture-coordinate count from metadata bits
`11:8`, caps A25's first loop at four, and places the remaining four formats
in A26.  It also updates internal per-coordinate component counts to 1, 2, 3,
or 4.  The separate point-sprite branch repacks the linkage and is not part of
the safe initial subset.

Concrete ordinary values are consequently:

```text
oPos + oD0                  A25=25f40000 A26=26000000
oPos + oD0 + oT0.xy         A25=25f4c000 A26=26000000
oPos + oD0 + oT0.xyzw       A25=25f4f000 A26=26000000
oPos + oD0 + oD1 + T0/T1.xy A25=25f6cc00 A26=26000000
```

These are complete A25/A26 words because the updater replaces all of their
low 24 bits.  A28 is different: only bits `18:17` may be derived from the VS
metadata.  Preserve all other bits from the current pixel-input/raster image.
For example, applying the oD0+oT0 rule to the proven `28ffcc00` base gives
`28fdcc00`; inventing the remaining A28 fields from A25 would overwrite
pixel-input linkage.

The generic activation transaction is now bounded exactly:

```text
# subtype 32 constants needed by the compiler finalizer, then any user runs
FE03043C 0000000C 32030400 CC000000
<12 finalizer-owned dwords>

# type-04 control, in vendor order
FE020440 0000043C 00040000 CC000000
A61_with_bool_mask  A62_with_compiler_low9  A60_with_active_last
CC000000

# upload count is independent of A60.active-last
FE03043C (upload_count * 4) 31030000 CC000000
<upload_count native records>
```

A60 is `0x60800000 | (active_count - 1)` for the initial absolute,
straight-line form.  A61 starts from the initialized control image, replaces
only Boolean bits `15:0`, and derives bits `23`, `22:20`, and `19` from object
`+0`, `+8`, and `+c` exactly as described above.  A62 preserves its initialized
upper bits and replaces only `8:0`.  Do not construct any of these words from
zero.  The vendor packet places A60 before the H3 upload, so retain that order;
the safety boundary is to emit no vertex/primitive packet until constants,
control words, and the complete program upload have all retired in the same
ordered command stream.

## Mesa implementation gates for glmark2 and ioquake3

The present Mesa driver executes the vertex NIR in Gallium `draw` and submits
post-transform vertices.  That path already covers matrix transforms,
lighting, clipping, culling, and arbitrary vertex ALU for gears.  A native VS
is therefore an optimization, not a prerequisite for the next application
milestone.  ioquake3's nearer functional blocker is two-texture
base-times-lightmap fragment state; its exact fixed program, x2/x4 overbright
derivatives, and guarded tests are in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  General glmark2
scenes additionally require fragment expressions beyond REPLACE/MODULATE, so
enabling a native VS must not be used to advertise a general GLSL fragment
compiler.

The first hardware VS lowerer can nevertheless be implementation-ready and
safe if it is an optional fast path with deterministic fallback to `draw`:

1. Accept NIR only after scalar/vector lowering into a single straight-line
   block.  Initially allow `mov`, `fadd`, `fsub`, `fmul`, `ffma`, `fdot3`,
   `fdot4`, `fmin`, `fmax`, `frcp`, `frsq`, `ffract`, `flt`, and `fge`.
   Their native selectors and ADD/MUL literal lowerings are already in the
   golden corpus.  Lower matrix products to DP4/DP3 sequences.  Keep LIT,
   EXP/LOG/POW, normalization, and SINCOS behind separate corpus and live
   gates even though their encodings are statically recovered.
2. Permit only source NONE/NEG and destination NONE/SAT.  Reject relative
   constants, address registers, integer/Boolean ALU, flow, predication,
   coissue, indirect input/output, vertex texture fetch, and every unresolved
   NIR intrinsic.  Rejection selects the existing software VS; it must not
   become a draw failure.
3. Allocate only `r0..r11`, `v0..v15`, `c0..c255`, and full 32-bit float
   values.  Require one dominating full-XYZW oPos write.  Initially accept at
   most oD0 and oT0.xy; reject partial position, oD1, fog, point size,
   two-sided colors, and extra generic varyings until their complete consumer
   state is enabled.
4. Count records after all lowering and after appending the position finalizer
   and two inactive sentinels.  The non-negotiable encoding checks are
   `1 <= active_count <= 512`, `active_count <= upload_count <= 512`, and
   `(active_count - 1) == A60[8:0]`; reject rather than mask on overflow.
   Use a conservative policy cap of 256 active and 256 uploaded records for
   the first Mesa path.  That cap is not a silicon claim; it stays below both
   the unchecked 512-wrap case and the vendor Linux object's 295-record
   overlap boundary.  Raise it only after bounded silicon sweeps.
5. Derive output metadata from the final NIR writes, build A25/A26 with the
   exact table above, patch only A28 bits `18:17`, carry the template's exact
   compiler-derived A62.low9, and retain independent active/upload counts.
   For the simple full-MOV textured path requiring oD0 plus oT0.xy, the
   expected metadata/A25/A26/A62-low tuple is
   `00000144 / 25f4c000 / 26000000 / 002`.
6. Keep the current proven H6 input declaration and post-viewport convention
   for the existing fixed-shader regression only.  A generic compiled shader
   writes raw oPos destination 32, after which its three active finalizer
   records read internal source 128, compute reciprocal W, and write raster
   position destination 33 using internal sources 224/225.  The Xorg
   post-transform shader instead writes destination 33 directly.  Feeding
   Gallium `draw`'s screen-space X/Y/Z and reciprocal-W tuple through the
   generic finalizer would therefore transform it twice.  A real object-space
   fast path needs a new bounded clip-space vertex-buffer ABI and declaration
   builder; the Windows declaration map proves float1/2/3/4 bytes
   `02/42/82/c2`, but this must not be smuggled into the current immediate
   post-TNL ioctl.

Use four explicit rollout gates.  Gate 0 compiles and validates only, dumping
the native records through
[`../../tools/chrome9_vs_decode.py`](../../tools/chrome9_vs_decode.py).  Gate
1 uploads constants, A25/A26/A28, A61/A62, and the native store but leaves
A60 invalid and launches no primitive; it must retire idle.  Gate 2 adds the
new clip-space input ABI and executes only full oPos/oD0 with W=1 under
guards, while retaining the 188-word post-transform path as an independent
regression rather than passing its vertices to the generic finalizer.  Gate 3
adds oT0.xy and a one-texture REPLACE draw with the exact tuple above.  Only
after all four pass should a qualifying NIR shader bypass `draw`; every other
shader continues on the already working software path.

## GL 2 / GLES 2 fragment path: bounded implementation order

A native PS 2.0 compiler is useful before it is sufficient for either API.
Mesa 26.2's desktop `compute_version()` requires the GL 1.5 chain (therefore
`ARB_shadow`) plus vertex shader, fragment shader, NPOT textures, separate
blend equations, and two-sided stencil for GL 2.0.  Its ES2 path requires the
two shader stages, NPOT textures, and separate blend equations.  Chrome9
currently clears both `texture_shadow_map` and `npot_textures`, so neither
version gate can open accidentally.  Keep those caps false while developing
the compiler; setting them to make a context appear is not shader validation.

The current kernel interface is another hard boundary.  It accepts only the
enumerated REPLACE and MODULATE fragment programs.  A Mesa NIR backend cannot
submit an arbitrary native PS until there is a bounded UAPI which carries or
constructs all of the following as one validated transaction:

* finalized ALU and TAU stores, at most 96 and 32 records respectively;
* the exact ordered ALU/TAU phase sequence from which the kernel can verify
  A90 start phase and derive A93/A94 and A95..A9a transition maps;
* one color output oC0, no depth output or TEXKILL in the first revision;
* float constants c0..c31, uploaded through the two API banks beginning at
  selectors `22030000` and `22030080` without exposing internal/gap banks;
* the sampler ids and fragment-input linkage consumed by the program, checked
  against the texture-state and VS-output transaction for the same draw.

Do not accept raw A90..A9b words from userspace.  Counts must equal the store
lengths, every transition bit must be below the corresponding count, unused
bitmap bits must be zero, and A9b must remain kernel-owned runtime state.
Likewise, a successful vendor compile is not validation: it silently
truncates overlong stores and allows unmapped texture-register sources to
become native id `3f`.

The first NIR-to-native fragment lowerer should be deterministic and fall
back to rejection, never to approximate execution:

1. Run ordinary NIR scalarization/copy propagation/DCE, lower `fsub` to ADD
   plus source NEG and `ffma` to MAD, then require a single acyclic block.
   Reject remaining loops, branches, phi nodes, calls, indirect constants,
   integer/Boolean ALU, derivatives, discard, depth output, and unsupported
   system values.
2. Initially allow full-precision MOV, ADD, MUL, and MAD with XYZW swizzles,
   NONE/NEG sources, NONE/SAT destination, r0..r11, c0..c31, and oC0.  Add
   DP3/DP4, MIN/MAX, RCP/RSQ, FRC, CMP/select, and every multi-record lowering
   only after a numerical silicon vector for that operation.  Preflight the
   expanded record count before building any packet.
3. Gate inputs independently.  First use only proven primary color v0.  Then
   add one ordinary TEXLD using TAU t0/s0 and its existing texture/linkage
   oracle.  A direct ALU read of texture coordinate t0, secondary color v1,
   or t1 is a new raster-linkage test even though its instruction field is
   decoded.  Never infer 64 varyings from the six-bit source id.
4. Gate scheduling shapes in increasing order: ALU-only; one leading TAU then
   ALU; two independent TAUs then ALU; finally a dependent
   TAU-to-ALU-to-TAU-to-ALU stream.  The exact control tuples for all four are
   documented above.  TEXLDP, TEXLDB, and TEXKILL remain later gates because
   only their compiler encodings, not their complete live state, are known.

For ioquake3, the lower-risk next step remains a fixed two-texture
base-times-lightmap enum using the already recovered two-stage state and
shader oracle; it does not require exposing GL 2.0.  For glmark2's GLES2
scenes, an internal allowlisted shader fast path can bring up individual
tests, but truthful ES2 exposure must wait for the full ES2 minimum texture,
blend, format, varying, shader-language, and draw semantics.  The proprietary
ICDs themselves did not advertise GLSL, so their compiler is an encoding
oracle rather than a conformance claim.

## Offline glamor compiler milestone

The matching T2 sysroot's `libglamoregl.so` has SHA-256
`11fb74c3196900f056657b1ecc1346d0b16440c215f45a62d88df6362507dcd3`.
Its embedded startup/copy/composite shader strings establish the first useful
GLSL expression family without guessing from a benchmark:

```glsl
gl_Position = v_position;
source_texture = v_texcoord0.xy;
mask_texture = v_texcoord1.xy;

frag_color = source;                         /* solid */
frag_color = texture(source_sampler, uv0);   /* copy */
frag_color = get_source() * get_mask();      /* composite */
```

`get_source` and `get_mask` independently select a uniform color or a 2-D
texture.  The adjacent variants multiply by mask alpha, source alpha, perform
RGBX reconstruction, repeat-coordinate repair, discard, or dual-source
output.  Those are separate compiler gates, not aliases of the full-vector
MUL path.

The offline-only Mesa-side compiler in
`chrome9_shader_offline.[ch]` now recognizes exactly fifteen finalized
fragment forms:

| normalized expression | constants | samplers | execution shape |
|---|---:|---:|---|
| `v0` | none | none | ALU-only |
| `c0` | c0 | none | ALU-only |
| `v0*c0` | c0 | none | ALU-only |
| `c0*c1` | c0,c1 | none | ALU-only |
| `tex0` | none | s0 | TAU0, ALU |
| `tex0*v0` | none | s0 | TAU0, ALU |
| `tex0*c0` | c0 | s0 | TAU0, ALU |
| `tex0*tex1` | none | s0,s1 | TAU0, TAU1, ALU |
| `v0*v1.aaaa`, `v0.aaaa*v1` | none | none | ALU-only |
| `c0*c1.aaaa`, `c0.aaaa*c1` | c0,c1 | none | ALU-only |
| `tex0*c0.aaaa`, `tex0.aaaa*c0` | c0 | s0 | TAU0, ALU |
| `vec4(tex0.rgb,1)` | driver c0.x=1 | s0 | TAU0, two partial ALUs |

Every table entry is the exact Linux compiler result after configuration-2
post-translation, including A90..A9b and the per-record dependency fields.
MUL inputs are canonicalized only for commutativity.  The compiler reports
the constant, varying, and sampler masks needed to cross-check draw state; it
has no raw-native-input entry point.  The RGBX form additionally marks c0.x
as driver-owned one; accepting a user value there would turn alpha forcing
into ordinary partial output.

The component-alpha differentials are exact and useful beyond the template
table.  Replicated alpha changes both halves of the split operand encoding;
it is not merely low swizzle `6db`:

```text
v0      * v1.aaaa : 0140b6d0 300416db 00f04005 00000820
v0.aaaa * v1      : 0140b6d0 b0041053 00f0406d 00000820
c0      * c1.aaaa : 0140b6d0 300816db 00f08005 00000820
c0.aaaa * c1      : 0140b6d0 b0081053 00f0806d 00000820
tex0    * c0.aaaa : 0140b6d0 b0200053 00f0806d 00000820
tex0.aaaa * c0    : 0140b6d0 b0080053 00f2006d 00000820
```

The ALU-only cases use `90058000/92000001`; one-texture cases use the normal
`90018000/91000001/92010001/93000001` tuple and TAU `301001b0`.  Direct
`tex0*tex1.aaaa` is deliberately absent: the generic Linux compiler erases
that temporary swizzle in its direct-output optimization.  The Xorg fixed
program has an exact scheduled `r0*r2.wwww` instruction, but its allocation
and two-ALU control image must remain an indivisible separate oracle rather
than being spliced into the generic program.

The exact RGBX execution candidate is:

```text
A90..A93 90018000 91000001 92010002 93000001
TAU       301001b0
ALU       01c00530 501c0053 00720005 000d0820  # oC0.rgb = tex0.rgb
          01c00530 801c0053 008080b6 000d0820  # oC0.a = c0.x
```

It is a compiler-exact partial-output sequence, not yet a silicon result.
The live discriminator must sample an XRGB texture whose stored alpha bytes
are deliberately non-one, bind c0.x=`3f800000`, and require unchanged RGB
plus output alpha `ff`.  Repeat with c0.x=zero to prove the second write is
active rather than an implicit render-target alpha fill.

The same file now carries eight exact Linux vertex templates: clip position;
position plus T0.xy; position plus T0/T1.xy; M4x4 position with zero, one, or
two texture coordinates; M4x4 position plus color and T0.xy; and the complete
M4x4 position plus color and T0/T1.xy form.  The
two-texture glamor pass-through has eight uploaded records, six active
records, metadata `00000204`, A62 low value two, and these three user
records before the three active finalizer records and two inactive sentinels:

```text
0008014c 00000000 00000000 00080a0f  # oPos = v0
0008414c 00000000 00000000 00080283  # oT0.xy = v1.xy
0008814c 00000000 00000000 00080293  # oT1.xy = v2.xy
```

This native vertex template takes clip-space input and runs the generic
reciprocal-W/viewport finalizer.  It must not consume the current Gallium
`draw` module's post-viewport vertices.  For the first glamor milestone,
retain software vertex execution plus the live-proven Xorg post-transform VS;
only the fragment classifier needs to select the new finalized programs.

For native clip-space execution, the M4x4+T0/T1 form is twelve uploaded
records, ten active records, metadata `00000204`, and A62 low value six.  Its
seven semantic records are exact:

```text
0008014c 0040014c 00000000 00060801
0008014c 0040414c 00000000 00060802
0008014c 0040814c 00000000 00060804
0008014c 0040c14c 0000014c 0034010f
0010014c 00000000 00000000 00080a0f  # oPos = transformed r0
0008414c 00000000 00000000 00080283  # oT0.xy = v1.xy
0008814c 00000000 00000000 00080293  # oT1.xy = v2.xy
```

It reads c0..c3 and v0..v2.  The three active reciprocal-W/viewport records
and two inactive sentinels shown earlier follow byte-for-byte.  Exact sibling
metadata is:

| native template | upload | active | object `+4` | A25 / A26 | A62 low | A60 |
|---|---:|---:|---:|---|---:|---:|
| M4x4 position | 10 | 8 | `00000004` | `25f00000 / 26000000` | 3 | `60800007` |
| M4x4 position + T0.xy | 11 | 9 | `00000104` | `25f0c000 / 26000000` | 5 | `60800008` |
| M4x4 position + T0/T1.xy | 12 | 10 | `00000204` | `25f0cc00 / 26000000` | 6 | `60800009` |
| M4x4 position + oD0 + T0.xy | 12 | 10 | `00000144` | `25f4c000 / 26000000` | 6 | `60800009` |
| M4x4 position + oD0 + T0/T1.xy | 13 | 11 | `00000244` | `25f4cc00 / 26000000` | 7 | `6080000a` |

The color-plus-texture templates use dense inputs: oD0 reads v1, T0 reads v2,
and T1 reads v3.  T0 must therefore begin `0008814c`, not the v1-source
`0008414c` used by the no-color T0 template.  A hardware-free compiler run
caught and corrected that distinction in the offline table.  The exact
identity-only UAPI proposal, compact declarations, address-free gates, and
guarded live sequence are in
[native-clip-vs-uapi-oracle.md](native-clip-vs-uapi-oracle.md).

### Safe native-shader live gates

Keep native VS and the offline PS compiler behind an opt-in debug path with
all public shader/API caps unchanged.  The first live sequence should be
strictly monotonic and keep the existing post-TNL path as the control:

1. Compile and byte-compare the selected key against this corpus.  Reject if
   ALU/TAU/VS counts, resource masks, output metadata, active/upload counts,
   or compiler-derived A62 value differ.
2. In an otherwise idle ring, emit the already-proven 2D-to-3D transition,
   exact internal viewport constants, c0..c3, A25/A26/A28, A61/A62, and the
   entire VS/PS stores, but omit A60 and every vertex/primitive word.  Require
   CR idle, engine status zero, error zero, target/source/guard bytes
   unchanged.  This is address-free except for already mapped constants and
   cannot rasterize.
3. Emit only the exact A60 active-last word in a second no-vertex submission.
   Require the same idle/status/guard result.  This isolates activation from
   fetch and raster launch.
4. Use a new explicitly clip-space vertex ABI, never the current post-viewport
   immediate tuple.  First run M4x4 position only with an identity matrix,
   W=1 finite clip coordinates, constant-color PS, COPY blend, no depth, one
   guarded target, and one triangle well inside the viewport.  Compare every
   covered pixel and guard against the existing software-draw/Xorg-VS result.
5. Add T0.xy with a 2x2 four-color nearest/clamp texture and finite [0,1]
   coordinates; then add T1.xy and the finalized two-texture MUL.  At each
   rung require idle/error zero, exact covered-pixel colors, immutable sources,
   and all guards before allowing the next key.

On any timeout or mismatch, disable the native key for the process and return
to software `draw`; do not retry a nearby program or reset merely to continue.
No live gate changes GL/GLSL/ES caps.  Capability exposure waits for ordinary
application-created NIR, constant updates, varying linkage, resource hazards,
and negative/rejection tests to pass through the same bounded path.

The standalone corpus builds without Mesa integration:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 \
  chrome9_shader_offline.c chrome9_shader_offline_test.c \
  -o /tmp/chrome9-shader-offline-test
/tmp/chrome9-shader-offline-test
```

It checks fifteen PS and all eight VS templates, exact instruction words,
controls, metadata, active/upload counts, resource masks, and a rejected
unsupported form.  This code is intentionally absent from `meson.build` and
does not change caps or submission.

The same corpus now has a Level-0 canonical-object verifier.  Its precise
count/resource/control/instruction/slack checks, recovered hard bounds, native
field hazards, and fail-closed NIR admission map are in
[native-shader-verifier-policy.md](native-shader-verifier-policy.md).

The NIR-facing gate may map to this key only after proving all of the
following: one entry point and one acyclic block; exactly one full-XYZW color
store; only identity MOV wrappers and one full-vector FMUL; leaves limited to
COL0, aligned vec4 c0/c1, and ordinary normalized 2-D TEX at linked T0/S0 or
T1/S1; no source swizzle, partial write, projection, bias/LOD, offset, shadow,
array, derivative, discard, depth, system value, integer, or indirect access.
The texture coordinate must be the matching full `Tn.xy` input.  Any unused
instruction or resource is rejection.  In particular, glamor's `.a` mask,
repeat-repair, gradient, stipple, glyph, and dual-source variants stay on
fallback until they receive their own native corpus and silicon gate.  The
now-encoded component-alpha and RGBX forms remain offline-only until their
varying/partial-write tests pass.

### Repeat math and discard boundary

Configuration-two finalization gives exact one-record semantic candidates
for three operations used inside glamor's coordinate-repair code:

```text
frc oC0,v0:       01c00530 301c0053 00f04005 000e0820
abs oC0,v0:       00400538 30040053 00f04005 00090820
cmp oC0,v0,c0,c1: 00810530 30080053 00f04005 00090820
```

All use the ALU-only `90058000/92000001` control shape.  These records prove
encoding only.  Glamor's repair helper also needs floor/modulo, comparisons,
integer repeat-mode selection, and conditional out-of-range zero, so accepting
FRC/ABS/CMP alone would miscompile it.  Ordinary normalized coordinates with
the already mapped sampler repeat/clamp/mirror state avoid that shader path;
large-pixmap `RepeatFix` remains software fallback.

The Linux compiler's target-finalized `texkill r0; mov oC0,v0` form is an
ALU-to-TAU-to-ALU schedule:

```text
A90..A95 90058000 91000001 92010002 93000001 94000000 95000001
TAU       314001b1
ALU       01c00530 301c0053 00f20005 000d0420
          01c00530 301c0053 00f04005 000d0820
```

Adding a leading `texld r0,t0,s0` produces two TAUs and controls
`90018000 91000001 92020002 93000003 94000000 95000001`; its stores are:

```text
TAU 201001b0 314001b1
ALU 01c00530 301c0053 00f00005 000d0420
    01c00530 301c0053 00f20005 000d0820
```

Neither form has silicon proof, and TEXKILL has side effects which make a
bad schedule riskier than ordinary color ALU.  Keep discard rejected.  For
the narrow stipple case where the fragment output alpha itself is the tested
texture/mask alpha, prefer the already live fixed alpha-test state and prove
`alpha > 0` under guards; it avoids enabling a new shader side effect.  This
does not implement discard based on a value different from output alpha.

This milestone still does not justify GL 2.0, ES 2.0, or glamor enablement.
It supplies compiler truth for solid/copy/full-RGBA composite only; NPOT,
FBO/render-to-texture, texture-domain synchronization, all required blend
equations, stencil, shader-language limits, and the remaining startup shaders
must pass independently.  A classifier miss must keep the existing safe
software path or report unsupported, never substitute a nearby template.

## Compiler feature limits and safe Mesa exposure

The Windows pixel frontend recognizes `ps_1_0` through `ps_1_4` and `ps_2_0`;
there is no recovered PS 3.0 path.  Its PS 2.0 tokenizer has a 128-record IR
limit.  The shared backend separately enforces the real native store limits:
96 ALU instructions and 32 texture-address instructions.  H5/Chrome9 HD
allocation exposes 12 ordinary temporaries, four special/spill temporaries,
and ten compiler texture slots.  Hardware/API exposure remains eight texture
units.  PS 2.0 has 32 API float constants.  Native destinations encode four
color outputs plus depth, but the OpenGL driver advertises one render target;
initial Mesa support must therefore use only `oC0`.

The vendor return status does not enforce those capacities.  Programs with
97..128 one-record MOVs report success but truncate at 96 native ALUs; 33/34
TEXLDs report success but truncate at 32 TAUs; a 129-MOV program crashes the
128-record tokenizer.  Count expanded native records in Mesa and fail before
96/32/128 is exceeded.

This limit needs an explicit validator.  The vendor frontend accepts `oC4`
and emits the exact native depth destination word `000d0920` without setting
the depth-present flag, while `oC16` wraps to the `oC0` native word and leaves
an impossible color-output count of 17 in metadata.  Accept at most native
`oC0..oC3`, and expose only `oC0` until MRT state is independently proven.

The PS 2.0 dispatcher is straight-line: it has no CALL, IF, or LOOP entries.
The tested Windows tokenizer crashes on a valid isolated SLT token, and its
isolated DP2ADD path emits nothing; Linux 5.75 supplies usable lowerings for
both.  These are compiler defects/limitations, not evidence that Mesa should
omit the operations: lower them explicitly.  Modifier encodings beyond NONE
and NEG disagree between driver generations and require numerical silicon
tests before use.

PS destination modifiers also need an allowlist: the Linux table maps every
odd code to saturate bit 12 and every even code to NONE.  Only NONE and SAT
are valid; reject the other parity aliases.

For PS result shift, raw codes 0,1,2,3 map to native identity/x2/x4/x8 and
codes 13,14,15 map to native /8,/4,/2.  Reserved codes 4..12 silently become
identity in the vendor frontend; reject them.

Do not infer predication or coissue from frontend acceptance.  Linux VS 2.0
emits identical native code when instruction-token bits 28, 29, 30, or 31 are
set; Linux PS 2.0 ignores bits 28..30 and rejects bit 31.  PS relative-constant
tokens are rejected.  These flags require explicit Mesa rejection or
lowering.

Native TEXLD carries a four-bit sampler id and the offline compiler maps
s0..s15 to `201001b0..2f1001bf`.  A single 16-sampler program produces 16 TAU
records and a T2A transition at bit 15 (`object+0x60=00008000`), confirming
that this is intentional rather than index leakage.  Its parser nevertheless
fails to range-check larger ids:
s16 gives `301001b0`, s32 aliases s0 as `201001b0`, and s255 corrupts the word
to `ff1001ff`.  Restrict the first Mesa implementation to the independently
advertised s0..s7 ceiling and mask only after validating the logical id.
The raw invalid-s16 word numerically collides with a valid finalized-s0 word:
bit 28 is an unchecked sampler-index carry in the former but a dependency bit
set by post-translation in the latter.  Native-word validation therefore
cannot recover the logical sampler id; range-check it before encoding.

The VS 2.0 compiler accepts arithmetic, matrix expansion, relative constants,
and structured flow emitters.  It has a 256-record normalized IR array and a
nine-bit hardware last-index field, but no checked native append limit was
found.  One API instruction can expand to several native instructions, so
neither 256 nor 512 is a proven safe hardware maximum.  Flow-control target
fixups, instruction latency, and branch behavior are not safe for hardware
execution yet.  A first Mesa compiler should emit straight-line VS code,
enforce its own conservative native bound, and use only cross-checked
instructions.

Static/offline flow fixups are now decoded enough to validate compiled output:
CALL/CALLNZ use dword 1 bits `18:10` for `target-call`; IF uses dword 2 for
`end-if` or `else+1-if`; ELSE uses dword 1 for `end-else`; LOOP and REP use
dword 2 for `end-start-1`.  All are masked to nine bits.  Vendor-side storage
has only 20 fixup records per construct class and neither table counts nor
out-of-range deltas are checked.  This does not make branching silicon-safe;
it gives the validator exact fields to forbid initially and range-check later.

The VS type-04 active last-index is nine bits, giving an encoded ceiling of
512 active native records.  This is not a silicon-proven capacity.  The vendor
object can hold only 295 native records and its 256-record normalized frontend
overruns first; initial Mesa exposure should remain below all three boundaries
with checked expansion accounting.  PS limits are firmer because its append
routines explicitly reject ALU index 96 and TAU index 32.

The boundary is demonstrably unsafe rather than merely unknown.  The vendor
compiler accepts 256 one-record MOVs and uploads 261 records including its
five-record epilogue/padding, but at 257 and 258 source records it overruns the
fixed normalized array and still reports success while losing/corrupting an
operation.  BREAK, BREAKC, and IFC can also be accepted into IR while emitting
no native operation.  The Linux VS/PS "validate" entry points only remap
resources and return success, and the old KMD does not inspect H3 program
payloads.  Mesa/DRM therefore need their own checked append bounds, supported
opcode/flow allowlist, packet bounds, timeout, and reset path.

The Linux user-mode `hwmHangDetect_inv` and ICD wrapper are both empty returns;
the old stack has no shader-fault recovery to inherit.  Its function named
`stmAppendHWException_invi` only emits padding/saved state and is unrelated to
fault detection.

Relative constants use one instruction-wide native selector: `a0.x/y/z/w`
map to selector nibbles 4/5/6/7 and `aL` maps to 8.  The vendor compiler accepts
two unequal relative selectors in one ADD and silently keeps the first, so a
new compiler must split or reject such an instruction.  No native VS
predication or VS 3.0 path is proven.  PS 2.0 is straight-line only.

## Render-target-to-texture ordering

The public register description and exact Xorg stream identify the texture
consumer invalidation packet.  H1 type-01 subaddress 01 bit 8 enables the
texture cache (`0` disables and clears it); normal Xorg state keeps it enabled
with payload `01c01540`.  H2 type-02 subtype FE subaddress 00 bit 0 is a
one-shot texture-cache clear.  Xorg emits it on every general texture-state
transaction:

```text
# one active texture: A00 bits 7:4 = 1, clear bit 0 = 1
FE020440 0000043C FE020000 CC000000
00000011 01002521 08010101 05000000
0D810000 CC000000 CC000000 CC000000

# zero active textures: clear bit remains set
FE020440 0000043C FE020000 CC000000
00000001 01002521 08010100 0D810000
```

Per-stage H2 type-02 subaddress 18 bit 23 forces a texture-cache miss, but the
normal one-texture oracle uses `18001100` with that bit clear.  It is not the
ordinary barrier.  For a rendered resource rebound as a texture, first retire
the producer in CR/fence order, then program its texture state and issue the
subtype-FE A00 clear before sampling.  The clear invalidates the texture
consumer cache; it does not substitute for producer completion, SF PTE/GTI
maintenance, or CPU DMA ownership synchronization.  Linux `stmFlushPipe_inv`
at `0x176260` is a literal zero-return stub, so its call sites provide no hidden
hardware render-cache flush.

Neither proprietary OpenGL ICD advertises GLSL, despite containing native
VS/PS compilers.  Native compiler capability is therefore evidence for a
Gallium/NIR backend, not evidence for advertising untested GL features.

## Evidence boundary and remaining unknowns

Silicon-proven facts are the full 188- and 232-dword streams, including their
exact activation order and output images.  Compiler-proven facts are the
constant MOV, decoded operand fields, constant-bank maps, and 96/32 PS limits.
The six-dword constant derivative is an intentionally small synthesis of those
two evidence sets and is not yet silicon-proven.

Still unknown or unsafe to synthesize are the complete meanings of PS controls
`90/91/93..9b`, all VS `61/62` resource fields for arbitrary compiled shaders,
PS dependency scheduling rules, the maximum safe VS native length, VS flow
runtime/delays, and nontrivial modifier numerical behavior.  Until those are
isolated, use finalized
compiler output inside a proven state envelope rather than constructing new
control words from partial field knowledge.
