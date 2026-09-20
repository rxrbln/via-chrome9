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

The compiler-coherent zero-runtime-state PS block is separately known:

```text
FE020440 0000043C 00010000 CC000000
90040000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9A000000 9B000000
```

Linux 5.75 reports object metadata `+2c=00040000`, ALU count one, TAU count
zero, and all other relevant fields zero; the Windows backend emits the same
native instruction.  This coherent control tuple is useful as a second
discriminator after the six-dword derivative, but it is not part of a
silicon-proven full draw and its packet termination/alignment must be emitted
by the normal H2 builder rather than copied as a naked 12-word array.

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
