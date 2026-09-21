# VIA 5.75 Chrome9 shader compiler reverse engineering

Status: executable offline vertex and pixel compilers recovered; core VS/PS
operand fields, register maps, lowerings, texture words, upload ABI, and
compiler/resource limits decoded.  VS flow-control displacement fields are
decoded statically/offline but are not silicon-tested; final scheduling bits
and silicon behavior remain incomplete.

This note covers the proprietary Linux DRI driver shipped in
`binary/5.75.32.87a-u910-55689/`.  It complements the newer Windows-driver
analysis in [shader-isa.md](shader-isa.md).  Addresses below are ELF virtual
addresses for this exact file:

```text
file:     bin/via_chrome9_dri.so
SHA-256: fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
build-id: f140f76cfb18e3f368245054629d1ef2e67be129
format:   ELF32 i386, ET_DYN, symbols and .debug_info present, not stripped
```

## Result

The binary contains both the Chrome9 hardware vertex/pixel compilers and the
unrelated CPU/SSE vertex-shader emulator.  The public symbols beginning with
`PVS_`, `PVSCP_`, `stmPPS_`, `PSOPT_`, and `INVPS_` are hardware compiler
code.  Symbols such as `CSSEVSCompiler`, `SSECodeCreator`, and source units
such as `SSEVSCompiler.cpp` are CPU emulation and must not be used to infer the
GPU ISA.

The hardware VS 2.0 parser and dispatcher can be called outside the driver.
The checked-in probe at
[`tools/re/chrome9_vs_compile_probe.c`](../../tools/re/chrome9_vs_compile_probe.c)
loads the DRI DSO and invokes:

```text
PVSCP_MSVSPARSER_VS20   0x001900b0
PVSCP_DISPATCHER_VS20_H5 0x0018e620
```

It requires no X server, DRM node, Wine, or GPU.  On the present 64-bit host,
32-bit execution is allowed through `qemu-i386`; empty compatibility DSOs are
enough for `libdrm.so.2` and `libexpat.so.1` because the compiler path does
not call either library.

For example:

```sh
mkdir -p /tmp/chrome9-probe-libs
gcc -m32 -shared -Wl,-soname,libdrm.so.2 \
  -o /tmp/chrome9-probe-libs/libdrm.so.2 tools/re/chrome9_stub.c
gcc -m32 -shared -Wl,-soname,libexpat.so.1 \
  -o /tmp/chrome9-probe-libs/libexpat.so.1 tools/re/chrome9_stub.c
gcc -m32 -o /tmp/chrome9-vs-compile-probe \
  tools/re/chrome9_vs_compile_probe.c -ldl
qemu-i386 -E LD_LIBRARY_PATH=/tmp/chrome9-probe-libs \
  /tmp/chrome9-vs-compile-probe \
  fffe0200 03000002 800f0000 90e40000 a0e40000 \
  02000001 c00f0000 80e40000 0000ffff
```

That token stream is `vs_2_0; add r0,v0,c0; mov oPos,r0; end`.  The first
native instruction emitted on this binary is:

```text
0008014c 00206490 0040014c 0004010f
```

The synthetic zero context causes the dispatcher to append five position
fixup instructions.  That is useful evidence that the full frontend and
hardware emitter ran, but those fixups must not yet be treated as the normal
OpenGL viewport setup.  The first ADD instruction is independent of them.

[`tools/re/chrome9_pvs_probe.c`](../../tools/re/chrome9_pvs_probe.c) is a
smaller differential oracle.  It calls a named `PVS_*` emitter directly with
the compiler's normalized tokens and dumps the resulting native words.  Raw
D3D tokens are not accepted by these emitters.  The full probe prints the
six-dword normalized IR records, so a parser result can be copied directly to
the small oracle.  It is useful for assigning one field at a time without
parser or position-fixup noise.

The pixel compiler can also run completely offline.  The checked-in
[`tools/re/chrome9_pps_probe.c`](../../tools/re/chrome9_pps_probe.c) reconstructs
the PS 2.0 callback table used by `stmCreatePixelShaderInstance_inv` and calls
the Linux DRI compiler under `qemu-i386`.  It needs neither Wine nor hardware:

```sh
gcc -m32 -Wall -Wextra -Werror -o /tmp/chrome9-pps-probe \
  tools/re/chrome9_pps_probe.c -ldl
qemu-i386 -E LD_LIBRARY_PATH=/tmp/chrome9-probe-libs \
  /tmp/chrome9-pps-probe
```

The default shader is `ps_2_0; mov oC0,c0; end`.  Both this Linux compiler and
the independently recovered Windows UMD compiler produce exactly:

```text
ALU count = 1, TAU count = 0
01c00530 301c0053 00f08005 000d0820
```

The Linux harness calls `stmInitializePixelShaderInstance_inv` at `0x1b81a0`,
then `stmPPSCompile_invi` at `0x1b5d00`.  Its callback table uses
`stmPPSTokenParser20_invi` (`0x1b6e10`), `stmPPSProcessPS20_invi`
(`0x1b8b80`), and `stmPPSValidatePS20_invi` (`0x1b5870`).  The register
callbacks and their exact H5 limits come from `stmPPSSetRegManager_invi`
(`0x1b5ad0`).

## What the shipped DWARF does and does not contain

`llvm-dwarfdump --debug-info --show-children=false` lists compilation units
only for the CPU shader emulator (`swemu.cpp`, `Dag.cpp`,
`SSEVSCompiler.cpp`, `SSECodeCreator.cpp`, and related files).  It provides
excellent types for software emulation but no hardware PVS/PPS C types.  The
hardware compiler remains recoverable because the ELF symbol table preserves
every C routine name and size and the command/update paths contain literal
packet selectors.

The hardware-related source-unit strings include:

```text
Stmvs_prog_H5.c
stmps_compiler_inv.c
stmvs_DagOpt_H5.c
PVSCP.c / PVSCPCpl.c / PVSCPParser.c
stmps_dagopt_inv.c
stm_staticshader_inv.c
stmShaderDepGraph.c
```

## Vertex compiler object and native program

The parser clears a `0x3a0c`-byte object.  Important offsets proven by the
parser, dispatcher, emitters, and `stmUpdateVS_inv` are:

| Offset | Meaning | Evidence |
|---:|---|---|
| `0x0424` | native instruction count | every `PVS_*` emitter and `stmUpdateVS_inv+0xbd` |
| `0x0428` | chip/context mode byte | dispatcher `0x18e6c1`-`0x18e6cd` |
| `0x042c` | first 16-byte native instruction | emitters and upload memcpy at `0x17c881` |
| `0x16a0` | first 24-byte parsed/normalized instruction record | parser `0x19027a`; dispatcher uses `+0x16a4` |
| `0x2ea4` | normalized instruction count | parser `0x190485`; dispatcher `0x18e646` |
| `0x39fc` | pointer to native compiler/output object | dispatcher `0x18e64c` |

Each native vertex instruction is exactly four little-endian dwords.  The
upload path at `stmUpdateVS_inv` (`0x0017c4c0`) calculates
`native_count * 4` dwords, emits selector `0x31030000`, and copies
`native_count * 16` bytes from object offset `0x42c`.

[`../../tools/chrome9_vs_decode.py`](../../tools/chrome9_vs_decode.py) is the
executable decoder for these records.  It names only the source, destination,
operation, saturation, and relative-address fields proved below and reports
every remaining set bit as unassigned.  Use it as the compile-only gate before
putting a newly lowered record in a packet; an unexplained set bit is a
validation failure, not an optimization hint.

The upload count is deliberately distinct from the active-program end index.
`stmUpdateVS_inv+0xbd..+0x2d0` reads `object+0x424` for the H3 payload length,
but reads `object+0x14 - 1` for type-04 `A60[8:0]`.  Offline VS 2.0 compiler
differentials make the distinction observable:

| source operations | uploaded records (`+0x424`) | metadata `+0x14` | `A60[8:0]` |
|---:|---:|---:|---:|
| one `MOV oPos` | 6 | 4 | 3 |
| `MOV oPos`; `MOV oD0` | 7 | 5 | 4 |
| `MOV oPos`; `MOV oD0`; `MOV oT0` | 8 | 6 | 5 |

In each case the final two uploaded records are identical tail records and lie
beyond the active end index.  Therefore `object+0x14` is active-program count
metadata, not an alias for the upload count.  A compiler/backend must retain
both fields; deriving `A60` from the H3 payload length would execute two records
which the proprietary compiler intentionally leaves inactive.
The 2015 Windows D3D uploader independently performs the same split at
`VTGUModeDX32A.dll:0x1015ac64..0x1015ac9a`: `+0x424` is retained as the H3
copy count while `+0x14-1` is masked into image word `+0x2260[8:0]`.  It then
emits `61,62,60` followed by an H3 payload sized from the retained `+0x424`
value (`0x1015aeff..0x1015afa8`).

For the straight-line MOV probes, the full layout is especially regular.  If
the frontend produces `N` native user instructions, the compiler appends three
active position/viewport finalizer records and then two identical inactive
tail records.  Thus `object+0x14=N+3`, `object+0x424=N+5`, and `A60=N+2`.
For a one-instruction shader the appended records are exactly:

```text
# active finalizer records
00201b6c 00000000 00000000 00280108
00200150 00101b6c 00201b6c 001c0107
0010014c 00380150 00384154 0004021f

# uploaded but outside A60's active range; repeated twice
0020014c 00000000 00000000 0008020f
0020014c 00000000 00000000 0008020f
```

The two extra records may be safe prefetch/sentinel padding, but that role is
an inference; their byte pattern, upload, and exclusion from the active range
are directly observed.  Retain them when reproducing compiler output.  The
three active finalizer records use the internal constant/register namespace,
so the fixed subtype-32 internal-constant upload is part of a generic compiled
VS transaction, not merely optional Xorg state.

The position destinations on the two paths are not interchangeable.  A user
`MOV oPos,v0` targets native destination 32 (`...00080a0f`).  The three active
finalizer records then read internal unified source 128, issue an RCP of W,
and finish with a MAD to native destination 33 (`...0004021f`) using internal
sources 224 and 225.  By contrast, the silicon-proven Xorg post-transform
shader's position MAD writes destination 33 directly.  This proves the
pipeline boundary even though selector 14 in the middle finalizer record is
not yet named numerically: destination 32 is pre-finalizer clip position and
destination 33 is the raster-facing position.

Consequently Gallium `draw` output cannot be fed to a generic compiled VS.
Its immediate tuple already contains viewport-transformed X/Y/Z and reciprocal
clip W; the generic finalizer would apply its reciprocal-W/viewport sequence a
second time.  Native NIR execution needs a clip-space vertex ABI plus the
matching internal constant and type-04 viewport image.  Until that exists,
generic VS compilation is a hardware-free validation target and the proven
Xorg shader remains the only execution path for post-transform vertices.

The object is larger than the hardware store because it also contains parsed
instructions, dependency/fixup tables, and metadata.  The normalized array is
exactly 256 records (`0x16a0` through `0x2e9f`, 24 bytes each).  The native
array begins at `0x42c` and has room for 295 complete 16-byte records before
the parsed array at `0x16a0`.  Those are object-layout capacities, not proven
silicon limits: the emitters do not perform an append bound check, one API
operation can expand to several native records, and the upload control word
only has a nine-bit last-index field.

An executable boundary sweep shows why neither capacity may be exposed as a
hardware limit.  Straight-line one-native-record MOV programs produce:

| source MOVs | normalized records | uploaded native records | metadata `+0x14` |
|---:|---:|---:|---:|
| 250 | 250 | 255 | 253 |
| 251 | 251 | 256 | 254 |
| 252 | 252 | 257 | 255 |
| 255 | 255 | 260 | 258 |
| 256 | 256 | 261 | 259 |

At 257 and 258 source instructions the parser still reports 257/258 records
and returns success, but only 261/262 native records result: input has already
overrun the fixed 256-record normalized array and adjacent compiler state is
being lost or corrupted.  There is no rejection at that boundary.  The
five-record straight-line overhead still fits the 295-record native area at
256 MOVs, but a source instruction such as POW expands to four records.  A new
compiler must bound both normalized and native appends before writing them; a
conservative native limit is a software policy until the silicon limit is
measured.

The type-04 shader-control last-index field is nine bits, so its architectural
encoding can name active indices 0..511 (at most 512 active records).  H3's
payload count is wider, but cannot make an instruction beyond that active
range executable.  This is an encoding ceiling, not proof that all 512 slots
exist or retire correctly on VX900.  The recovered vendor compiler reaches a
stricter practical boundary first: 256 normalized source records and 295
native-array records, both unchecked as described above.

The Windows compiler object used by the headless wrapper has a separately
allocated native array, so it exposes the nine-bit failure even when the
Linux object's overlapping native area would already have overflowed.  A
shader containing 169 NRM instructions followed by full oPos and oD0 writes
parses as 171 source records (`parsed+0x2ea4=0xab`) and returns success with:

```text
native+0x14 = 00000202    # 514 active records
native+0x424 = 00000204   # 516 uploaded records
```

`stmUpdateVS_inv:0x17c593..0x17c5b2` explicitly computes
`(active_count - 1) & 0x1ff`, so last index 513 becomes A60 index one.  The
compiler neither truncates nor rejects the program.  Input token SHA-256 is
`5cc5add5e6cc504625285d609f3884e1c47e5b57834b93e6db03212feae1c85a`;
the 516-record native blob SHA-256 is
`0ca5e97c11dc630daffb0ec3ce51beccc22ed4fca9b7be8ff5673e7fe16c20d9`.
This is a deterministic offline compiler result, not a hardware test.  Mesa
must reject after native lowering/finalization unless both active count and
total upload count fit the 512-record architectural store, and may choose a
lower conservative cap until silicon execution bounds are tested.

## Vertex native instruction layout

The following fields are established by direct emitter differentials and by
the shifts/masks in `PVS_ADD` (`0x00182d80`).  The three source slots occupy
dwords 0, 1, and 2.  The destination and operation control occupy dword 3.

### Source operand word

| Bits | Meaning | Confidence |
|---:|---|---|
| `0` | source negate | high; `SUB` differs from `ADD` only here for source C |
| `4:2` | W swizzle/literal selector | high |
| `7:5` | Z swizzle/literal selector | high |
| `10:8` | Y swizzle/literal selector | high |
| `13:11` | X swizzle/literal selector | high |
| `22:14` | unified nine-bit register index | high |

Selector values 0, 1, 2, and 3 mean X, Y, Z, and W; values 4 and 5 are
literal one and zero.  Values 6 and 7 remain unassigned.  For example, the
identity source is `0x0010014c`; the four replicated swizzles are:

| Swizzle | Word |
|---|---:|
| `xxxx` | `00100000` |
| `yyyy` | `00100924` |
| `zzzz` | `00101248` |
| `wwww` | `00101b6c` |

`PVS_GetSrcRegType_H5` at `0x00181a30` maps normalized D3D register types to
a unified register number.  The observed bases are:

| Normalized type | D3D meaning | Native index base | Native operand base |
|---:|---|---:|---:|
| `0` | TEMP | 64 | `00100000` |
| `1` | INPUT | 32 | `00080000` |
| `2` | float CONST | 256 | `00400000` |
| `7` | CONSTINT | 96 | `00180000` |
| `14` | CONSTBOOL | 112 | `001c0000` |

TEMP and INPUT identifiers are masked to five bits.  Float constants preserve
all eight identifier bits: VS 2.0 `c0..c255` fill native classes 8 through 15,
and `c255` produces unified index 511.  CONSTINT `i0..i15` and CONSTBOOL
`b0..b15` share native class 3 as ids 0..15 and 16..31.  LABEL preserves its
id but returns native class -1; the remaining normalized types 0..18 are
rejected.  CONST2/3/4 are normalized by the parser before the emitter;
passing their raw D3D type values directly to a `PVS_*` routine is not a
valid test.

Consequently invalid source `r32`/`v32` aliases `r0`/`v0`.  For Mesa's first
compiler, keep logical vertex inputs within `v0..v15` and temporaries within
the 12-register allocation budget rather than treating the mask as a usable
capacity.

That 256-float limit also requires a frontend check.  The native source field
is only nine bits, so directly encoding invalid normalized `c256` computes
unified index 512 and wraps to index 0 (`0000014c`), crossing out of the
constant classes entirely; `c511` becomes unified index 255.  Reject VS float
constant indices above 255 before mapping.

The direct emitter does not enforce the API ranges for integer or Boolean
constants.  Both occupy fixed native class 3; it adds low-id bases 0 and 16
and masks the result to five bits.  Thus `i16` aliases native `b0`, `b16`
aliases native `i0`, and identifier 32 wraps to the corresponding id zero.
The uploader still has
only 16 packed integers and A61 still has only 16 Boolean bits, so these are
invalid aliases, not extra storage.  Mesa must reject `iN`/`bN` for `N>=16`
before native register mapping.

The six-dword normalized IR source token is also decoded: bits `10:0` are the
identifier, `15:11` the normalized type, `23:16` the D3D swizzle, `27:24` the
source modifier, and `30:28` the relative-address selector.  Current emitters
consume modifier bit 0 as negate.  A full-parser sweep of all D3D modifier
codes 0..13 proves that even codes emit the unchanged native operand and odd
codes set only native operand bit 0.  Thus NEG works, but the unchecked parser
silently aliases BIASNEG, SIGNNEG, X2, DZ, ABS, and NOT to NEG and aliases
BIAS, SIGN, COMP, X2NEG, DW, and ABSNEG to NONE according to enum parity.
These are not supported semantics: a new compiler must permit only NONE and
NEG on VS 2.0 operands and reject or lower every other source modifier.

### Destination/control word

| Bits | Meaning | Confidence |
|---:|---|---|
| `3:0` | XYZW write mask | high |
| `9:4` | unified six-bit destination index | high |
| `14` | saturate result | high |
| `21:17` | primary operation selector | high |
| `27:24` | instruction-wide relative-address selector | high |

`PVS_GetDstRegType_H5` gives the exact map: TEMP becomes native class 1;
ADDR becomes class 0; RASTOUT ids 0, 1, and 2 become class-2 ids 0, 6, and 7
(`oPos`, `oFog`, `oPts`); ATTROUT ids 0..3 become class-2 ids 2..5; and
TEXCRDOUT becomes class 2 at id `8 + n`.  Other destination types are
invalid.  Normal destinations store `class * 16 + id` in bits `9:4`.
The TEMP destination id is masked to four bits: invalid `r16` aliases `r0`.
The VS 2.0 API/compiler budget is 12 temporaries, so Mesa should allocate only
`r0..r11` until the four spare native ids have an independently proven role.
The normalized destination modifier is equally unchecked: a full-parser
sweep of codes 0..15 maps only modifier bit 0 to native dword-3 bit 14.  Code
1 (SATURATE) is valid; every other odd code aliases SATURATE and every other
even code aliases NONE.  Mesa must not copy this permissive behavior.
`MOVA` is special and emits fixed low control value `0x11`, for example:

```text
0008014c 00000000 00000000 000a0011
```

Full parser/dispatcher MOV differentials give the initial varying map.  These
include finalizer-owned output-class bits that a bare `PVS_MOV` call omits:

| D3D output | Native MOV destination/control | Unified index |
|---|---:|---:|
| oPos | `00080a0f` | 32 |
| oD0 / oD1 | `0008422f` / `0008423f` | 34 / 35 |
| oFog | `0008426f` | 38 |
| oPts | `0008027f` | 39 |
| oT0 / oT7 | `0008028f` / `000802ff` | 40 / 47 |

All use `v0` source word `0008014c`.  The compiler's metadata at object `+4`
is an exact output-linkage descriptor:

- bit 2 marks oPos active (the generic compiler/finalizer sets it even in the
  isolated non-position output probes);
- bits 5, 6, 7, and 13 mark oPts, oD0, oD1, and oFog respectively;
- bits 11:8 contain the highest active texture-coordinate output count, so
  oT0..oT7 produce 1..8;
- each oTn has a two-bit dimension field at bits `16+2n` through `17+2n`.
  Encodings `3,0,1,2` mean 1D, 2D, 3D, and 4D respectively.

Thus isolated full-width outputs produce oPos `00000004`, oPts `00000024`,
oD0/oD1 `00000044/00000084`, oFog `00002004`, oT0 `00020104`, oT1
`00080204`, oT2 `00200304`, oT3 `00800404`, oT4 `02000504`, and oT7
`80000804`.  For oT0 specifically, x/xy/xyz/xyzw produce `00030104`,
`00000104`, `00010104`, and `00020104`.  This metadata is not itself a
program-store word; it must feed VS-output/PS-input linkage state.

The vendor frontend does not reject texture-coordinate outputs beyond the
real `oT0..oT7` linkage space.  `oT8` emits destination/control `0008020f`
(wrapping out of the class-2 varying range) and loses the texture-output
metadata; `oT15` similarly emits `0008027f`, while one tested `oT16` token
crashed the offline compiler.  These are validation failures, not additional
varyings.  Mesa must cap vertex texture-coordinate outputs at eight before
calling the encoder.

Observed primary operation selectors are:

| Selector (`dword3[21:17]`) | Operation or lowering |
|---:|---|
| `2` | MAD datapath; ADD, SUB, MUL, MAD, and several lowered ops |
| `3` | DP datapath; DP3 and DP4 differ in source swizzles |
| `4` | MOV |
| `5` | MOVA/address write |
| `7` | FRC |
| `8` | MAX |
| `9` | MIN |
| `10` | SGE |
| `11` | SLT |
| `12` | SGN |
| `16` | EXP |
| `17` | EXPP |
| `18` | LOG |
| `19` | LOGP |
| `20` | RCP |
| `21` | RSQ |
| `22` | final SINCOS operation |
| `24` | LIT |
| `26` | matrix final dot/output marker |

The normalized direct-emitter sweep also establishes native expansion counts:

| API operation | Native instructions | Principal selectors |
|---|---:|---|
| MOV, ADD, SUB, MUL, MAD | 1 | `4`; `2` for the arithmetic family |
| DP3, DP4 | 1 | `3` |
| EXP, EXPP, LOG, LOGP | 1 | `16`, `17`, `18`, `19` |
| FRC, LIT, RCP, RSQ | 1 | `7`, `24`, `20`, `21` |
| MAX, MIN, SGE, SLT | 1 | `8`, `9`, `10`, `11` |
| ABS, DST, SGN | 1 | `8`, `2`, `12` |
| CRS, LRP, SINCOS | 2 | lowered sequences |
| NRM | 3 | DP3, RSQ, MAD |
| POW | 4 | MAX, LOG, MAD, EXP |
| M3x2/M3x3/M3x4 | 2/3/4 | DP sequence plus final-output marker |
| M4x3/M4x4 | 3/4 | DP sequence plus final-output marker |
| NOP | 1 | sixteen zero bytes |

The OpenGL-only direct emitters add three useful exact lowering oracles not
reachable through the D3D VS 2.0 dispatcher:

```text
DPH r0,v0,c0: 00080150 0040014c 00000000 0006010f
FLR r0,v0:    0008014c 00000000 00000000 000c010f
ARL a0,v0:    0008014c 00000000 00000000 000c080f
               0020014c 00000000 00000000 000a0011
```

`PVS_DPH_ICD @0x18d720` is the native DP selector with the first operand's W
forced to one.  `PVS_FLR_ICD @0x18da80` exposes primary selector 6.  The
`PVS_ARL_ICD @0x18d490` sequence applies that selector into internal unified
temporary 128 and then MOVA-writes `a0.x`.  These compiler-backed words are
appropriate golden vectors for a Mesa lowering, but like the flow records
they remain unexecuted on silicon.

These values come from clean one-operation emitter outputs.  They do not mean
each API opcode is one native instruction.  Important lowerings observed by
the direct oracle include:

```text
ADD: MAD(a, encoded-one, b)
SUB: same as ADD with source-C negate bit set
MUL: MAD(a, b, encoded-zero)
ABS: MAX(a, -a)
NRM: DP3 + RSQ + MAD (three instructions)
CRS: two MAD-path instructions
LRP: two MAD-path instructions
POW: four instructions (MAX, LOG, MAD, EXP)
```

Examples, in byte order as emitted:

```text
MOV r0,v0:
4c 01 08 00  00 00 00 00  00 00 00 00  0f 01 08 00

ADD r0,v0,c0:
4c 01 08 00  90 64 20 00  4c 01 40 00  0f 01 04 00
```

The middle ADD source word `0x00206490` is the compiler's encoded constant
one.  The MUL lowering uses `0x00206db4` as encoded zero in its third slot.
Their four three-bit selectors are respectively all 4 and all 5.  The
compiler happens to put unified source 129 in the otherwise ignored register
field, but one/zero come from the literal selectors, not an internal vector.
This is a compiler-backed lowering contract, not an API-visible constant.

Direct `PVS_SWZ_ICD` calls separate those selectors from the compiler's
special unified id.  With ordinary input `v0` (native id 32), the control
bytes below produce:

| control | native source | meaning |
|---:|---:|---|
| `04040404` | `00082490` | literal `(1,1,1,1)` |
| `05050505` | `00082db4` | literal `(0,0,0,0)` |
| `14141414` | `00082491` | negated literal one |

Thus selector 4/5, not register id 129, is sufficient, and operand bit 0
negates a literal exactly as it negates a register source.  These three
vectors are retained in the executable golden corpus.

Relative VS 2.0 constant addressing is fully assigned at the frontend/native
boundary.  Full-token-parser MOV probes using `c5[a0.x/y/z/w]` normalize raw
relative tokens `b0000000`, `b0550000`, `b0aa0000`, and `b0ff0000` to relative
modes 1..4 and native dword-3 nibbles 4, 5, 6, and 7.  The existing `c0[aL]`
probe selects mode 5/native nibble 8 and uses raw relative token `f0000800`;
absolute mode 0 emits nibble 0.  These modes are independent of the constant
index.

The native selector is instruction-wide.  A deliberately malformed but fully
parsed ADD with `c0[a0.x]` and `c1[a0.y]` is accepted, but emits only selector
4: the first nonzero source selector silently wins.  A compiler must reject or
split operations whose relative sources cannot share one selector.

The control-flow emitters are not standalone functions: most dereference
nesting and label-patch state in the compiler object.  Calling CALL, ENDIF,
LOOP, ENDLOOP, REP, ENDREP, LABEL, or RET on a zeroed object faults.  This is a
harness precondition, not evidence of a hardware fault.  Full parser/dispatcher
samples establish these initial records:

```text
REP  i0:    00180154 00280400 00300000 00380000
LOOP aL,i0: 00180154 00280400 00300000 00380000
IF   b0:    001c014c 00280400 00300000 003a0000
CALL l0:    0020a490 00280400 00300400 003a4000
CALLNZ l0,b0:
            001c014c 00280000 00300400 003a4000
```

ENDREP, ENDLOOP, and ENDIF patch compiler state/earlier records rather than
necessarily appending a record.  Compiler differentials for both IF and REP
set bit 2 of the final body instruction's dword 3 (`...0100` to `...0104`),
which is a strong boundary/end-marker candidate.  IF/ELSE adds an ELSE record
`0020a490 00280000 00000000 003a0000`.  LABEL is a fixup marker and does not
itself append a record; the exact RET sequence association is described below,
but remains untested on silicon.

More importantly, BREAK and BREAKC inside LOOP parse into normalized IR but
append no native record.  IFC likewise parses but emitted no native record in
the tested dispatcher path.  The compiler returns success rather than
diagnosing these unsupported operations.  PS 2.0 has no CALL/IF/LOOP dispatcher
entries at all, and no VS 3.0 or native predication path was recovered.  Mesa
must lower/select these constructs or reject the shader; none of these
flow-control patterns is approved for hardware execution yet.

Instruction flag validation is also absent.  For VS 2.0, setting instruction
token bit 28 (D3D predication), bit 29, bit 30 (coissue), or even bit 31 on an
otherwise ordinary MOV produces byte-identical native code; only bit 31 is
retained in normalized IR and the dispatcher still ignores it.  The PS 2.0
frontend likewise emits byte-identical MOV code with bits 28, 29, or 30 set;
bit 31 makes that frontend reject the stream.  PS 2.0 relative-constant source
tokens are rejected in the offline compiler.  These observations prove only
silent flag loss, not predication or coissue support.  A Mesa-side validator
must reject predicated instructions and unsupported instruction flags before
native emission.

The mandatory `PVSCP_POSTPROCESS_H5` pass resolves the supported flow records.
Every target/extent is a nine-bit value in bits `18:10`; the pass clears with
`0xfff803ff`, masks the computed delta with `0x1ff`, and shifts it by ten.
Its exact tables inside the parsed object are:

| Construct | count/table | Patched field and value |
|---|---|---|
| CALL/CALLNZ | count `+0x37a0`; 20 triples at `+0x36b0` `{call,target,label}` | CALL dword 1 = `target-call` |
| IF, no ELSE | count `+0x3898`; 20 triples at `+0x37a8` `{if,0,end}` | IF dword 2 = `end-if` |
| IF/ELSE | same table `{if,else,end}` | IF dword 2 = `else+1-if`; ELSE dword 1 = `end-else` |
| LOOP | count `+0x393c`; 20 pairs at `+0x389c` `{start,end}` | start dword 2 = `end-start-1` |
| REP | count `+0x39e0`; 20 pairs at `+0x3940` `{start,end}` | start dword 2 = `end-start-1` |

For example, one-instruction bodies after final postprocessing are:

```text
IF b0; MOV; ENDIF:
001c014c 00280400 00300800 003a0000

IF b0; MOV; ELSE; MOV; ENDIF:
001c014c 00280400 00300c00 003a0000
0020a490 00280800 00000000 003a0000

REP i0; MOV; ENDREP (LOOP aL,i0 is identical here):
00180154 00280400 00300400 00380000

CALL l0 with label two native records later:
0020a490 00280800 00300400 003a4000
```

In the minimal `CALL l0; MOV oPos,v0; LABEL l0; RET` probe, LABEL appends
nothing and the first RET appends this four-record sequence before the two
inactive upload tails:

```text
0010014c 00000000 00000000 0008020f
00101b6c 00000000 00000000 00280808
00100150 00201b6c 00101b6c 001c0807
0020014c 00380150 00384154 0004021f
```

CALLNZ uses the same dword-1 target field; with the same two-record forward
target its finalized first record is
`001c014c 00280800 00300400 003a4000`.  These exact compiler outputs still do
not establish the hardware call-stack depth or return latency.

The table storage proves a vendor software capacity of 20 occurrences of each
construct class, not a silicon nesting limit.  None of the emitters or the
postpass checks those counts or rejects a delta beyond nine bits; excess table
entries overwrite adjacent compiler state and long deltas wrap modulo 512.
The compiler harness now exposes the real postpass behind the
`CHROME9_POSTPROCESS` environment switch and allocates the two required
context blocks at their actual sizes.  A new compiler should generate and
range-check its own fixups rather than reproduce these unchecked tables.

## Shader validation boundary

The proprietary stack provides no native-ISA safety boundary.  Linux
`PVSCP_VALIDATE_VS20_H5` at `0x18fc80` updates constant metadata/dirty flags,
calls `PVS_ReMapInputReg_H5i`, and unconditionally returns zero.  It does not
check opcodes, flow nesting, branch targets, program length, or native-array
bounds.  `stmPPSValidatePS20_invi` at `0x1b5870` gathers register usage,
configures the 8/12-register manager, invokes its allocation callback, and
unconditionally returns one; it performs no native-ISA validation.

The old Linux KMD is no backstop.  `via_chrome9_check_header3()` skips groups
of four dwords until the next word which resembles an AGP header and never
examines the explicit H3 count or instruction payload
(`via_chrome9_verifier.c:801..812`).  The CPU fallback merely copies the stated
count to ports `043c/0440` (`via_chrome9_dma.c:880..895`).  A new DRM must use a
bounded packet parser and accept programmable code only from a trusted,
checked compiler/validator, with bounded completion and reset recovery.

The Windows KMD does not add ISA validation: its header-3 decoder at analysis
`0x000a0db0` honors the payload count and streams opaque groups of at most four
dwords through `0440..044c`, but does not decode VS/PS instructions.  Windows
therefore improves packet bounds, not shader semantic safety.

There is no useful user-mode hang recovery either.  The exported
`hwmHangDetect()` merely dispatches through the hardware vtable, while both
Chrome9 implementations recovered here, `hwmHangDetect_inv` at `0xac440` and
`hwmHangDetect_icdwrapper` at `0xa8b30`, are empty returns.  The similarly
named `stmAppendHWException_invi` is not fault handling: at `0x177370` it pads
to eight dwords with `CC000000` and appends selected saved state words.  It
does not inspect shader status or exceptions.  Silent compiler acceptance can
therefore reach an unbounded legacy wait; command timeout/reset is a mandatory
new-DRM property, not optional hardening.

## Vertex program setup and upload packets

`stmUpdateVS_inv` emits a type-02 control stream immediately before the
type-03 program packet:

```text
fe020440 0000043c 00040000 cc000000
state[0x2240] state[0x2244] state[0x223c]
<alignment padding>
fe03043c (instruction_count * 4) 31030000 cc000000
instruction_count * 16 bytes
```

The three control words are constructed from compiler metadata:

- native object `+0x14 - 1` goes to state word `0x223c[8:0]` and is the
  active-program last index, not universally the uploaded-record count minus
  one;
- native object `+0x10` goes to state word `0x2244[8:0]`;
- object `+0 <= 8` controls `state[0x2242].bit7`;
- object `+0x08 & 7` controls `state[0x2242].bits6:4`;
- object `+0x0c != 0` controls `state[0x2242].bit3`.

Hardware-free full-compiler differentials assign native object `+0x10`
directly as the low A62 resource value.  For the simple full-MOV subset it
equals the number of non-position output vectors: oPos only gives zero;
adding one of oD0 or oT0 gives one; oD0+oD1, oT0+oT1, and oD0+oT0 each give
two.  It is not universally an output count.  A four-row M4x4 position plus
full oPos MOV gives three with no non-position output; adding T0.xy gives
five, and adding both T0/T1.xy gives six.  Partial writes and the compiler's
position temporaries therefore contribute to this opaque resource value.
The Windows uploader at `VTGUModeDX32A.dll:0x1015aca0..0x1015acb7`
independently performs the same masked A62-low-nine update.  This means the
hand-built rectangle value `A62.low9=1ff` is not a valid generic-compiler
default, and Mesa must carry the compiler-derived value rather than recompute
it from output declarations.  The complete A25/A26 dimension packing and
implementation gates are recorded in
[shader-upload-oracles.md](shader-upload-oracles.md).

`stmUpdateIntBoolConst_inv` at `0x17baa0` closes the independent integer and
boolean upload contracts.  Sixteen vertex integer constants are stored as
records beginning at context `+0x4798` with a 20-byte stride.  For each `iN`,
the low byte of X, Y, Z, and W is packed into one little-endian dword:

```text
packed_iN = (x & ff) | ((y & ff) << 8) |
            ((z & ff) << 16) | ((w & ff) << 24)
```

Dirty bits come from context `+0x48dc`.  The uploader rounds the first dirty
index down to a group of four, emits counted H3 selector
`33030000 | (first_index >> 2)`, and sends packed dwords through the last dirty
index.  There are exactly 16 source constants `i0..i15`; the vendor path
silently truncates each API component to eight bits.

Boolean constants are not H3 payloads.  `b0..b15` reside at context
`+0x4738 + 6*N`; their low bytes are packed as one bit each into a 16-bit mask
and written into the low half of saved type-04 state word A61 at
`state+0x2240`.  `stmUpdateVS_inv` then emits A61 together with A62 and A60
before the VS program.  Thus native unified source ids 112..127 read the mask
in A61[15:0]; there is no separate boolean program-store selector.

The 2015 Windows D3D UMD independently implements the same contract in one
routine at `VTGUModeDX32A.dll:0x101593b0..0x10159507`.  Its integer half emits
exactly

```text
fe03043c 00000010 33030000 cc000000
16 packed dwords
```

at `0x10159413..0x1015948c`.  It walks 16 records at device-context
`+0x4a90`, stride `0x14`, and loads the bytes at record offsets `+0,+4,+8,+c`
in X/Y/Z/W order before producing the same little-endian packed dword.  This
Windows path uploads the complete 16-dword store rather than the Linux dirty
run, which also proves that selector `33030000` names integer group zero and
that its low selector bits advance in groups of four.

The Windows boolean half at `0x101594aa..0x10159500` walks four groups of four
bytes at device-context `+0x4a30`, with a six-byte stride between successive
booleans, ORs them into bits 0..15, and writes the resulting low 16 bits to
saved state `+0x2264`.  This is the Windows layout-equivalent of the Linux
saved A61 word at `state+0x2240`; both paths preserve its upper 16 control
bits.  Neither uploader masks a Boolean byte to one bit before shifting, so
the state setter/compiler must canonicalize values to 0 or 1.

These are masked updates to initialized state, not complete values.  A zeroed
offline context loses mandatory parameter-address bits.  In particular, Xorg
emits `60800003` for its four-instruction no-texture H6 VS, while static DRI
blit paths emit `60800004` for five instructions (`via_chrome9_dri.so` at
`0x1f304c` and `0x237a94`).  For those hand-built rectangle programs the low
nine bits equal the uploaded instruction count minus one.  That convenient
equality does not hold for a general compiler object, as the table above
shows.  Bit 23 is valid and high byte `60` is required.  Never upload the
synthetic zero-context value `00800003`.

Vertex float constants use selector family `0x3203xxxx`.  A fixed 12-dword
internal-constant upload at `0x17c6a5` is:

```text
fe03043c 0000000c 32030400 cc000000
12 dwords
```

The main programmable path also has the known 256-`vec4` full upload and
dirty-run updates in `stmPVSUpdateDirtyConst_H5` (`0x0017bc60`).  Integer
and boolean details are given above.

### Fixed-function lighting allocation

The old OpenGL fixed-function builder statically unrolls at most eight lights.
`stmFFVSLightCompute_H5i:0x1b00b8..0x1b0117` tests eight enable bits from
builder byte `+1`, extracts two mode bits per light from word `+2`, and routes
mode 1 to `stmFFVS_PointSpotLight_H5i(...,0)`, mode 2 to the same function
with final argument one, and mode 3 to `stmFFVS_DirectionalLight_H5i`.
Unsupported/zero mode emits no per-light body; there is no runtime native
shader branch for the light type.

`stmFFVS_UpdateLightConst_H5i:0x1afde0..0x1afedf` assigns light `i` ten
private float constants in two runs:

```text
c(135 + 3*i) .. c(137 + 3*i)
c(159 + 7*i) .. c(165 + 7*i)
```

Across `i=0..7`, the blocks are c135..c158 and c159..c214.  The producer at
`stmFFVSUpdatePreCalcConst_H5i:0x176b58..0x176cae` makes the first run
semantic rather than merely an allocator artifact.  For each `0x74`-byte
D3D-style light record it writes three componentwise color products:

| constant | light record | material/context | meaning |
|---|---:|---:|---|
| `c(135+3*i)` | `+0x24` | `+0x4f54` | ambient product |
| `c(136+3*i)` | `+0x04` | `+0x4f44` | diffuse product |
| `c(137+3*i)` | `+0x14` | `+0x4f64` | specular product |

Each row is one four-float vector.  Two members of the second run are now
assigned directly from the update code.  At `0x176d53..0x176db4`,
`c(162+7*i)` is produced from the light direction at record `+0x40` using the
matrix at driver context `+0x300`, followed by the vector-normalization
callback.  At `0x1772c1..0x1772ff`, `c(163+7*i)` is produced from the light
position at record `+0x34` using that same matrix.  Thus the seven-vector run
is currently:

```text
c159..c161 + 7*i    alternate ambient/diffuse/specular color triplet
c162       + 7*i    transformed, normalized light direction
c163       + 7*i    transformed light position
c164       + 7*i    point/spot distance-attenuation vector
c165       + 7*i    spot-cone vector
```

The labels for c159..c161/c164/c165 come from exact emitter use, not guessed
payload components: internal flags `ff` select c159..c161 in the same three
color positions where flags `c0` use the pre-multiplied c135..c137 triplet;
point and spot bodies consume c164 in their common distance path, while only
spot consumes c165 in its eleven-record cone extension.  The per-component
operand contract is partly visible too.  The common path reads c164 as
`(y,z,w,0)` (`00690a74`) and separately as `xxxx` (`00690000`): X is the
range/cutoff scalar and YZW are the three attenuation-coefficient slots,
although any CPU-side numerical preprocessing remains unresolved.  The spot
extension reads every c165 component separately: negated Z (`00695249`), W
(`00695b6c`), Y (`00694924`), and X (`00694000`).  Setter differentials are
still required to name their exact precomputed cone formulas.  This
80-constant private layout is relevant to reproducing the vendor fixed
function compiler, but it is not required by the current CPU draw-module TNL
path.

[`tools/re/chrome9_ffvs_light_probe.c`](../../tools/re/chrome9_ffvs_light_probe.c)
calls either one per-light emitter or the complete light accumulator under
`qemu-i386`.  It supplies the exact ten class/id pairs above and no GL, DRM,
or hardware context.  The probe executable used here has SHA-256
`df8bfca08baae4182a7f6e7daca9be77071ed7f6fe8d25b9858607282ea34ec0`.
The minimal builder flag `0x80` emits these four native records:

```text
0061c14c 00206490 0010814c 0004012f
00104154 00688155 00000000 00060141
0011014c 00000000 00000000 0030016f
0062014c 00118924 0010814c 0004012f
```

Their little-endian stream SHA-256 is
`4ab828df4b487875c23e375390a9f7a6bd8acbd13afc489595ad217531874970`.
Adding builder bit `0x40` (`flags=0xc0`) expands the exact body to 13 records:

```text
0061c14c 00206490 0010814c 0004012f
00206d95 00000000 00000000 0008015f
00114154 00206490 00688155 0004015f
00114154 00114154 00000000 00060158
00115b6c 00000000 00000000 002a0158
00114154 00115b74 00206db4 0004015f
00104154 00688155 00000000 00060141
00104154 00114154 00000000 00060142
0011014c 00000000 00000000 0030016f
0062414c 00119248 00206db4 0004015f
00110924 00618000 00000000 00140168
0011414c 00119b6c 0010c14c 0004013f
0062014c 00118924 0010814c 0004012f
```

That stream hashes to
`db3bdb101aa051ee8551e13fcea0a450e27baf116fd9b342a27ff7f04cfb442b`.
The same probe selects the point/spot emitter with its exact fourth argument.
With internal flags zero, point mode emits ten records:

```text
0068c154 00206490 00100155 0004016f
00118154 00118154 00000000 0006015f
00115b6c 00000000 00000000 002a0158
00116150 0011638c 00206db4 0004015f
00114154 00690a74 00000000 00060144
00111248 00000000 00000000 00280144
00115248 00690000 00000000 00160151
00111248 00114000 00206db4 00040144
00118154 00115b74 00206db4 0004016f
0061c14c 00111248 0010814c 0004012f
```

Its stream SHA-256 is
`002c6e4e8a68af8b983b793fcc3cbb3816c8d6441238471a8e652864d4cb8fdc`.
Spot mode retains those distance/attenuation records and inserts eleven cone
records before the final dot/product:

```text
0068c154 00206490 00100155 0004016f
00118154 00118154 00000000 0006015f
00115b6c 00000000 00000000 002a0158
00116150 0011638c 00206db4 0004015f
00114154 00690a74 00000000 00060144
00111248 00000000 00000000 00280144
00115248 00690000 00000000 00160151
00111248 00114000 00206db4 00040144
00118154 00115b74 00206db4 0004016f
00688155 00118154 00000000 00060151
00114000 00206490 00695249 00040152
00114924 00206db4 00000000 00100152
00114924 00695b6c 00206db4 00040152
00114000 00694924 00000000 00160154
00115248 00114924 00206490 001a0152
00114924 00000000 00000000 00240154
00115248 00694000 00206db4 00040154
00115248 00000000 00000000 00200154
00114924 00115248 00206db4 001a0154
00111248 00115248 00206db4 00040144
0061c14c 00111248 0010814c 0004012f
```

The 21-record spot stream SHA-256 is
`60971cb6d4b6ddbd61dc5dd5ee1e36106e3fa22d0f3434f954259d1f91835fae`.
At flags `0x80`, the point and spot variants contain 14 and 25 records, with
hashes `f5ea29c83a879aca78ca60f95eb5bd01627db521187da48a48b308824ae5251f`
and `60080707718195b609f5c3a1a22e68b89bf8d11ab026c4d0b04f1b1abf02b5a2`.
These flags are internal fixed-function-builder inputs, not API capability
bits.  An isolated flag sweep, unified-register identities, and the complete
`stmFFVSLightCompute_H5i` wrapper now assign the byte rather than merely
bounding its structural effects:

| bit | exact compiler effect |
|---:|---|
| `0x01` | lighting enable; tested by `stmFFVSCompile_UnTransform_H5i` before calling the complete light builder |
| `0x02` | local-viewer specular; on a `0xc0` light it inserts DP3/RSQ/scale records which normalize the eye vector before constructing the half vector |
| `0x04` | source emissive material from the per-vertex material temporary rather than the fixed material constant |
| `0x08` | source ambient material from the per-vertex material temporary; changes c135 to raw light-ambient c159 and introduces the vertex multiply |
| `0x10` | source diffuse material from the per-vertex material temporary; changes c136 to raw light-diffuse c160 and introduces the vertex multiply |
| `0x20` | source specular material from the per-vertex material temporary; on the `0x40` path changes c137 to raw light-specular c161 and introduces the vertex multiply |
| `0x40` | emit the specular/secondary-color light path |
| `0x80` | emit the primary ambient/diffuse light path |

The accumulator prologue is an independent proof for bits 2 and 3.  With
flags `00`, it initializes the primary accumulator from c130.  Flags `04`
use `c129 + vertex_emissive`; flags `08` use
`global_ambient * vertex_ambient + c131`; flags `0c` use
`global_ambient * vertex_ambient + vertex_emissive`.  The constant producer
at `0x176a40..0x176b19` computes c129 as fixed-material ambient times global
ambient and c130 as that value plus fixed-material emissive.  Thus the four
forms are algebraically consistent, not labels inferred from record count.

The complete one-directional-light streams for flags `00,04,08,0c,80,c0,c2,ff`
contain `4,4,5,5,7,20,22,25` records and hash respectively to
`215624b1`, `e802300d`, `af6f4a3f`, `0fcbf908`, `d0fe0172`, `b4d3371b`,
`bb4db8aa`, and `8f3f9b65` (first eight SHA-256 digits).  At flags `c0`, the
complete point and spot streams contain 31 and 42 records, hashes
`9119ba1c` and `e83f8d08`; local viewer (`c2`) raises those to 33 and 44,
hashes `00652403` and `3dd88e02`.  These wrapper results include accumulator
initialization and output MOVs, so they are the appropriate whole-light
goldens for a future Mesa fixed-function lowering.

For the isolated directional emitter, the same mapping produces these exact
structural deltas:

| flag delta | effect in `stmFFVS_DirectionalLight_H5i` |
|---:|---|
| `0x02` | adds two local-viewer eye-vector records, but only on the `0x40` specular path |
| `0x08` | selects the per-vertex ambient-material operands; count unchanged |
| `0x80` | enables the four-record primary path instead of returning after ambient |
| `0x40` | with `0x80`, expands four records to thirteen by adding specular |
| `0x10` | adds one per-vertex diffuse-material record |
| `0x20` | adds one per-vertex specular-material record, only with `0x40` |

The corresponding isolated-emitter raw stream hashes for flags
`80,88,90,a0,c0,c8,d0,e0,f0` are respectively
`4ab828df`, `67a39939`, `fa655924`, `4ab828df`, `db3bdb10`, `9279a576`,
`d73cde6b`, `ecc28540`, and `8a00d505` (first eight SHA-256 hex digits).
This table and the complete-wrapper hashes above provide both narrow and
whole-program regression vectors for the assigned state key.

The fixed-function output object has a stricter storage hazard than the
general 512-record A60 encoding ceiling.  Its native records begin at
object `+0x454`, while the temporary/register map begins at `+0x1554`, leaving
exactly `(0x1554-0x454)/16 = 272` records.  None of the three per-light
emitters checks this bound.  The probe's repeat mode demonstrates the
overflow without any GPU access:

| repeated emitter | flags | returned count | stream SHA-256 |
|---|---:|---:|---|
| 8 directional | `c0` | 104 | `b3a064409767a0c044e27089eda1807af97d03404c893585218da873df00f597` |
| 8 point | `c0` | 192 | `49725fa21bc3959fd207157915b6f7e8d2ff009bcceb4fd106695dadcaeebd3c` |
| 8 spot | `c0` | 280 | `13db77f60a9eb9e59bcbc6a29612ac48659106e1e64c7fb68f0a27ee77f6d952` |
| 8 spot | `ff` | 320 | `ff7177e5de8d9c11f54ea45260189969e7959a06c8936caa35a20c37718fcde2` |

Record index 272 starts exactly at `+0x1554`; the eight-spot `c0` form
therefore overwrites the first eight records' worth of allocator state and
still returns success.  A complete fixed-function shader also emits transform,
texture-coordinate, output, and optional fog records before/after the light
bodies, so 272 is an object-storage ceiling, not a usable light-only budget.
Any future Mesa native fixed-function compiler must count before every append
and reject the whole shader before touching this boundary.  The present CPU
draw-module TNL path avoids the vendor bug entirely.

The probe's `full_MODE N` form also invokes the real wrapper once with `N`
enabled lights, so it includes accumulator setup/output and exercises the
allocator map after every light.  Eight-light results make the failure mode
more concrete:

| complete wrapper | flags | records/result |
|---|---:|---:|
| directional | `c0` | 111, SHA-256 `ceed8578...` |
| directional + local viewer | `c2` | 127, SHA-256 `18daa8af...` |
| point | `c0` | 199, SHA-256 `e196ad62...` |
| point, all material-source bits | `ff` | 240, SHA-256 `bc2cf7d8...` |
| spot, primary only | `80` | 203, SHA-256 `1b56a39b...` |
| spot | `c0` | 287, returns after overwriting the allocator map, SHA-256 `a67ff726...` |
| spot + local viewer | `c2` | 303, returns after a larger overwrite, SHA-256 `12c91c32...` |
| spot, all flags | `ff` | process faults inside the wrapper after record storage crosses the map |

Therefore even the vendor API maximum of eight lights is unsafe for a
perfectly ordinary all-spot state key.  A replacement must preflight the
whole shader, including prologue, every enabled light, transform/texcoord/fog,
and epilogue; checking a per-light count after emission is too late because
the allocator metadata has already been consumed or corrupted.

## Pixel program object, limits, and upload

For the compiled pixel-program view, the Linux binary confirms the same
layout recovered from the newer Windows UMD:

| Offset from compiled object | Meaning |
|---:|---|
| `0x14` | pointer to 16-byte ALU instructions |
| `0x18` | ALU instruction count |
| `0x20` | pointer to 4-byte texture-address instructions |
| `0x24` | texture-address instruction count |

`INVPS_AddAInst` at `0x0019a290` refuses an append when the existing count is
greater than `0x5f`: the ALU store limit is 96 instructions.  `INVPS_AddTInst`
at `0x0019a1e0` similarly tests against `0x1f`: the texture-address store
limit is 32 instructions.  These are hard append limits, not initial vector
capacities.

They are not propagated as compile failures.  Offline programs containing 97
through 128 one-native-record MOVs return success with `ALU count=96`; the
tail is silently discarded.  Likewise 33 or 34 TEXLD records return success
with `TAU count=32`.  The normalized PS IR accepts 128 source instructions,
but a 129-MOV input crashes the compiler.  A Mesa compiler must bound both
source IR and post-lowering native ALU/TAU counts and fail compilation instead
of relying on these append routines.

`stmInitializePixelShaderInstance_inv` (`0x001b81a0`) clears `0xf0` bytes,
sets `object+0x10` to the object itself, creates an ALU dynamic array with
16-byte elements and initial capacity 32, and creates a TAU array with
4-byte elements and initial capacity 11.  It stores the backing pointers and
counts at the offsets above.  These capacities are distinct from the 96/32
append limits.

`stmPPSSetRegManager_invi` gives exact H5 logical resources for chip selectors
5..8 and 14: 12 ordinary temporaries (native ids 0..11), four special/spill
temporaries (ids 12..15), and ten texture slots.  The corresponding backend
callback-table fields at `+0x1354`, `+0x1358`, `+0x135c`, and `+0x1360`
contain 12, 4, 10, and 12.  Normal temporary allocation searches only 0..11;
the special path searches 12..15.  Texture allocation begins at the
general-register index 32.
The older selector-4 backend instead exposes 16 general temporaries and no H5
texture pool, so its limits must not be substituted for Chrome9 HD.

The data section also exposes exact native defaults:

```text
g_INVPS_AInstInit  @ 00427ce0:
  01c00530 301c0053 00f00005 00000020
g_INVPS_AInst2Init @ 00427cf0:
  0140b6d0 d0140924 00f140b6 00100020
g_INVPS_TInstInit  @ 00427d00:
  201001b0
```

Their semantic roles beyond "initial/default instruction" still require
differentials; they are nevertheless exact hardware bit patterns.

`stmUpdatePS_inv` at `0x0017f710` emits the following control and program
sequence:

```text
fe020440 0000043c 00010000 cc000000
90xxxxxx 91xxxxxx 92xxxxxx 93xxxxxx
94xxxxxx 95xxxxxx 96xxxxxx 97xxxxxx
98xxxxxx 99xxxxxx 9axxxxxx 9bxxxxxx

fe03043c (alu_count * 4) 20030000 cc000000
alu_count * 16 bytes

fe03043c tau_count 21030000 cc000000
tau_count * 4 bytes
```

The twelve control words are assembled at `0x17fcb9`-`0x17fd8f` from these
compiled-object fields:

| Control | Source |
|---:|---|
| `90` | `object+0x2c OR object+0x6c`; `+0x2c.bit18` selects an ALU-first program |
| `91` | `object+0x70` |
| `92` | `object+0x18 OR (object+0x24 << 16)`; ALU and TAU counts |
| `93`,`94` | `object+0x60,+0x64`; TAU-to-ALU transition bitmap |
| `95`..`9a` | `object+0x48..+0x5c`; ALU-to-TAU transition bitmap |
| `9b` | runtime state masked to 24 bits |

This decodes the most important previously unknown control word: `92` carries
both program-store lengths, ALU in the low half and TAU in the high half.

The scheduling controls are now exact too.  The compiler's
`INVPS_ContextSwitchA2T{Get,Set,Clear}` routines at
`0x00198150..0x001981db` treat `object+0x48` as an array of six 24-bit bitmap
words.  ALU instruction index `i` maps to word `i/24`, bit `i%24`; a set bit
means switch from the ALU store to the TAU store after that instruction.
Those six words are emitted as `A95..A9a`.  The corresponding T2A routines at
`0x00198320..0x001983ab` use `object+0x60` with the same packing.  The TAU
limit of 32 requires two words, emitted as `A93,A94`.

`INVPS_AddTInst` and `INVPS_AddAInst` at
`0x0019a1e0..0x0019a34d` establish the execution model:

- compiler phase `object+0x68` is 0 before the first instruction, 1 in ALU,
  and 2 in TAU;
- starting in ALU sets `object+0x2c.bit18`, hence `A90.bit18`; starting in TAU
  clears it;
- appending TAU while phase is ALU sets A2T bit `ALU_count-1`;
- appending ALU while phase is TAU sets T2A bit `TAU_count-1`.

Pixel execution therefore alternates between two separately uploaded stores,
with `A90.bit18` choosing the first store and `A93..A9a` defining exact switch
points.  These are not resource counts or generic dependency flags.  The
proprietary append limits mean only A2T indices 0..95 and T2A indices 0..31
are reachable, although six A2T words are present in the hardware tuple.

There is a mandatory pass after ordinary compile/validate.  The production
call chain at `stmCreatePixelShaderInstance_inv:0x1b923f..0x1b9247` invokes
`stmPPS_PostTranslationSetting_invi(object+0x10, context[0xa08]>>4)`.  Target
configuration value two is proved by exact convergence with both Xorg
oracles:

| program | pre-pass `object+2c` | configuration-2 `object+2c` | Xorg A90 |
|---|---:|---:|---:|
| ALU-only MOV | `00040000` | `00058000` | `90058000` |
| TEXLD then ALU | `00000000` | `00018000` | `90018000` |

For the texture case the pass also sets `object+70=1`, keeps T2A bit zero set,
changes TAU `201001b0` to `301001b0`, and changes the consuming ALU source
word `00f00005` to `00f20005`.  These are finalizer-owned dependency fields,
not sampler or register-class changes.  Together they give exact controls
`90018000 91000001 92010001 93000001`.  Do not upload output from
`stmPPSCompile_invi` directly; the earlier offline probe deliberately stopped
before this phase.

The same target finalizer is now measured for both two-TAU schedule shapes.
Two independent samples followed by one MUL produce TAUs
`a01001b0 311001b1`, ALU
`0140b6d0 3003f000 00f20005 00000820`, and controls
`90000000 91000001 92020001 93000002`.  Making the second coordinate depend
on the first result produces TAUs `201001b0 b11001b1`, ALUs
`01c00530 301c0053 00f00005 000d0420` and
`0140b6d0 3003f000 00f20005 00000820`, plus controls
`90000000 91000001 92020002 93000003 94000000 95000001`.
Consequently TAU bits 31/28 are schedule-dependent finalizer outputs, not
fixed opcode bits, and finalizing every TAU by a constant OR is wrong.

Adjacent compiler metadata is also pinned down:

| object offset | meaning | evidence |
|---:|---|---|
| `+0x68` | current/final phase: 0 empty, 1 ALU, 2 TAU | append routines; ALU-ending and TAU-ending probes |
| `+0x74` | original D3D shader version token | `ffff0200` in every PS 2.0 probe |
| `+0x78` | depth-output-present flag | `INVPS_HwRebuildInstBuffDesc`; `mov oDepth,c0` gives 1 |
| `+0x7c` | TEXKILL-present flag | TAU operation-class test in rebuild; TEXKILL probe gives 1 |

`stmUpdatePS_inv+0x69..+0xad` copies `object+0x78` into context-state byte
`+0x4e.bit7` and marks dirty group `0x00080000` when it changes.  Therefore a
depth-writing fragment shader needs this non-program-store state transition in
addition to its native ALU instruction (`...000d0920`).  A color-only shader
leaves the flag clear.  The exact emitted H2 subaddress for that context byte
still needs isolation; do not infer that uploading the depth-output ALU alone
enables depth export.

Pixel float constants use selector `0x22030000 | (start_dword & 0xffff)`.
The two 64-dword banks start at `0x00` and `0x80`; the two 28-dword banks
start at `0x40` and `0xc0`.  For PS 2.0, the upload copies IEEE-754 dwords
directly.  Legacy PS 1.x paths call `INV_FloatToC1_Float` at `0x001a7210`,
which clamps magnitudes with exponent at least 127 to signed 1.0 before
upload; that conversion must not be applied to PS 2.0 constants.

The bank split also fixes the PS 2.0 constant map.  Selector `22030000`
uploads 64 dwords to native constant ids 0..15 (API `c0..c15`), while
`22030080` uploads API `c16..c31` to native ids 32..47.  The 28-dword banks at
`22030040` and `220300c0` occupy internal ids 16..22 and 48..54.  No upload
exists for ids 23..31 or 55..63.  Out-of-range API tokens at `c32` and above
wrap/collide in the proprietary frontend and are not evidence for additional
programmable constants.  The exact first failures are especially hazardous:
`c32..c47` encode native ids 48..63, colliding with the second internal/gap
bank, and `c48..c63` wrap to native ids 0..15, aliasing API `c0..c15`.
Mesa must reject PS float-constant indices above 31 before emission.

The Windows D3D path independently fixes the software source offsets.  Its
combined PS update/build routine starts at `VTGUModeDX32A.dll:0x1015b000`.
For PS 2.0, `0x1015c117..0x1015c43b` emits the same four banks:

| Selector | Count | Source |
|---:|---:|---|
| `22030000` | `40` dwords | driver view `+4bd8` |
| `22030080` | `40` dwords | driver view `+4cd8` |
| `22030040` | `1c` dwords | `[driver view+9c34] +100` |
| `220300c0` | `1c` dwords | `[driver view+9c34] +300` |

The two internal banks are gated by dirty words `+400` and `+404`.  A narrow
path emits four dwords from `+2e54`/`+2e64` under selectors `2203005c` and
`220300dc`, gated by `+2e50`.  The older-than-PS-2.0 branch beginning at
`0x1015be4b` applies scalar conversion routine `0x1020ab00`; the PS 2.0 branch
copies API constant dwords directly.  This removes any ambiguity about whether
the 64/28 split was peculiar to the Linux OpenGL stack.

## Pixel ALU and texture ISA coverage

The following ALU fields are independently reproduced by the Linux and
Windows offline compilers.  Source names A/B/C are encoding slots, not source
language order.

| Field | Meaning | Coverage |
|---|---|---|
| dword 0 `3:0` | source-B modifier | varied |
| dword 0 `15:13,12:10,9:7,6:4` | source-B XYZW three-bit swizzle/literal selectors | varied |
| dword 0 `21:16`, `24:22` | source-B id, class | temp/input/constant/texture |
| dword 0 `31:28` | source-C modifier | varied |
| dword 1 `2:0,5:3,8:6,11:9` | source-C WZYX three-bit swizzle/literal selectors | varied |
| dword 1 `17:12`, `20:18` | source-C id, class | varied |
| dword 1 `27:24` | source-A modifier | varied |
| dword 1 `30:28`; dword 1 bit 31 plus dword 2 `1:0` | source-A W and split Z three-bit selectors | varied |
| dword 2 `4:2,7:5` | source-A YX three-bit selectors | varied |
| dword 2 `13:8`, `16:14` | source-A id, class | varied |
| dword 2 `23:20` | destination XYZW mask | varied |
| dword 3 `9:6`, `11:10` | destination id and class | temp and color output varied |
| dword 3 bit 12 | saturate | proven |
| dword 3 `15:13` | result scale | x2/x4/x8 and /2,/4,/8 proven |
| dword 3 `19:16` | ALU sub-operation | table below |

All PS ALU swizzle selectors are three bits, as in the VS ISA.  Values 0..3
select X/Y/Z/W, value 4 is literal one, and value 5 is literal zero.  The ADD
lowering's source-C word `30140924` has four selector-4 fields; the MUL
lowering's source-B word `0140b6d0` has four selector-5 fields.  This assigns
the bits previously mistaken for gaps and avoids allocating constants solely
to synthesize zero or one.  Values 6 and 7 remain unassigned.

For source-A identity operands at logical index zero, MOV differentials give
the compact class encodings directly in dword-2 bits `16:0`: temporary
`000005`, interpolated input `004005`, float constant `008005`, and
texture/misc `00c005`.  Changing c0 to c5 produces `008505`, and v0 to v3
produces `004305`, confirming the adjacent six-bit id.  Texture logical ids
are register-manager remapped; their post-allocation native id is not
guaranteed to equal the API id.

The interpolated-input field is exactly six bits, but that is not a usable
varying limit.  Offline Linux compiler sweeps produce source-A fields
`004005`, `005f05`, `006005`, and `007f05` for `v0`, `v31`, `v32`, and
`v63`.  `v64` aliases `v0`, `v127` aliases `v63`, and `v128` again aliases
`v0`; the frontend merely masks the unchecked token id to six bits.  No
corresponding 64-slot raster-linkage proof exists.  A new compiler must bound
inputs to the VS/PS linkage actually emitted and tested rather than advertise
64 varyings from this operand field; the first bounded subset uses only `v0`
and the TAU's `t0`.

Native destination class 0 is temporary.  Destination class 2 ids 0..3 are
`oC0..oC3`, and id 4 is depth.  Their dword-3 values are `000d0820`,
`000d0860`, `000d08a0`, `000d08e0`, and `000d0920`.  Compiled object `+0x30`
stores the highest color-output count (1..4), not a bitmask; `+0x44` is the
logical temporary-write mask, `+0x40` the texture-register usage mask, and
`+0x48` is set by TEXKILL because TEXKILL's lowering switches from ALU
instruction zero to TAU; a dependent second texture lookup can cause the same
ALU-to-TAU switch.  These fields feed final control state but are not
instruction dwords.

The PS frontend does not enforce that destination boundary.  `oC4` encodes
the same native class/id and exact dword `000d0920` as depth while leaving the
depth-present flag `object+0x78` clear; `oC16` wraps its native destination to
the `oC0` word while recording an impossible color-output count of 17 at
`object+0x30`.  Valid native color ids remain only 0..3, and current GL
exposure remains only `oC0`.  Reject higher color outputs before encoding.

Additional whole-program differentials constrain the otherwise opaque pixel
control metadata without guessing its final bit semantics:

| program shape | `+2c` | `+40` | `+44` | `+48` | `+60` |
|---|---:|---:|---:|---:|---:|
| ALU-only `MOV oC0,c0` | `00040000` | 0 | 0 | 0 | 0 |
| one sample to `r0`, then output | 0 | 1 | 1 | 0 | 1 |
| independent samples to `r0`,`r1` | 0 | 3 | 3 | 0 | 2 |
| second sample dependent on `r0` | 0 | 1 | 3 | 1 | 3 |
| TEXKILL plus output | `00040000` | 0 | 1 | 1 | 1 |

The table is an exact validation of the transition maps.  One TAU followed by
ALU sets T2A bit 0 (`A93=1`); two independent TAUs followed by ALU set only bit
1 (`A93=2`); inserting an ALU dependency between the TAUs sets T2A bits 0 and
1 (`A93=3`) plus A2T bit 0 (`A95=1`).  TEXKILL plus output starts in ALU
(`A90.bit18=1`) and sets both transition bit zeroes.  `A90`, `A93..A9a` must
come from finalized scheduling, not be reconstructed from instruction counts.
`object+0x6c` and `+0x70` stayed zero in this differential set and remain
semantically unresolved.

The one-operation PS 2.0 corpus gives the sub-operations and expansions below.
The exact input and output dwords are retained in
[`via-5.75-shader-corpus.tsv`](via-5.75-shader-corpus.tsv).
After building the two probes, all 117 current vectors can be replayed with:

```sh
python3 tools/re/chrome9_corpus_check.py
```

| D3D operation | Native count | dword-3 sub-operation(s) |
|---|---:|---|
| ADD, SUB, MAD, MUL | 1 | `0` |
| DP3, DP4 | 1 | `2`, `3` |
| EXP, LOG, RCP, RSQ | 1 | `4`, `5`, `6`, `7` |
| ABS, CMP | 1 | `9` |
| MAX, MIN, MOV, FRC | 1 | `b`, `c`, `d`, `e` |
| LRP | 2 | `0`, `0` |
| SGE | 2 | `0`, `9` |
| POW | 3 | `5`, `0`, `4` |
| CRS | 2 | `0`, `0` |
| NRM | 4 | `2`, `9`, `7`, `0` |
| SINCOS | 3 | `d`, `0`, `f` |

The two otherwise unnamed internal opcode holes are now resolved exactly.
Linux `fp_parse` reduction-table entries 41/42 create internal opcode 105
(`0x69`) for SIN and entries 31/32 create opcode 106 (`0x6a`) for COS.  Their
position among the adjacent statically named scalar operations is
`COS, EXP, LOG, RCP, RSQ, SIN, SINCOS`.  `stmPPSDispatcherPS20_invi` passes
mode 1 for SIN and mode 0 for COS to `stmPPS_SIN_COS_ICD_PS20_invi`; Windows
does the same at `VTGUModeDX32A.dll:0x101dccb4/0x101dccc7`.  The ordinary D3D
SINCOS path remains a combined three-record lowering: x/y/xy destination-mask
differentials only alter the last record's native writemask and do not select
these internal modes.

The Windows UMD lowers this same LRP token stream to one native sub-operation
`11`; the Linux 5.75 compiler lowers it to two sub-operation-0 instructions.
Both are compiler-backed encodings, but this cross-version difference is a
warning not to assume that every newer UMD lowering is valid on every Chrome9
revision.

The Windows private tokenizer crashes on the tested PS 2.0 SLT token (module
RVA `0x1dc0f7`) before an instruction reaches the dispatcher.  Isolated
DP2ADD returns success but creates no ALU/TAU output there.  Static analysis
shows that its Windows emitter is an adjacent-instruction pattern combiner
with strict X/Y masks and swizzles; one reverse-neighbor check uses a
0x20-byte stride although tokenizer records are 0x24 bytes, making it
unreachable in the recovered harness and possibly a vendor bug.  The Linux
5.75 compiler instead explicitly lowers isolated DP2ADD to two instructions:

```text
00820530 00081000 00f08000 00000020
00000530 90081249 00f08024 00000020
```

A new compiler should use an explicit lowering rather than depend on the
Windows peephole.  Result-scale code 0 is identity; 1, 2, and 3 are x2, x4,
and x8; 5, 6, and 7 are /2, /4, and /8.  Saturate is independent bit 12.
The same headless-Wine sweep completed and produced output for the other 23
of 25 Windows probes, so these two cases are localized frontend failures, not
a general Wine or harness failure.

The raw D3D destination-shift sweep fixes the validator mapping too.  Input
codes 0,1,2,3 map to native scale 0,1,2,3; input codes 13,14,15 map to native
7,6,5 (/8,/4,/2).  Reserved input codes 4..12 are silently accepted as scale
zero.  Mesa must allow exactly `{0,1,2,3,13,14,15}` and reject the reserved
codes.

The Linux frontend's `stmPPS_GetALUSrcMod` at `0x161d10` maps D3D source
modifier 1 (NEG) to native value 8 and passes every other nonzero four-bit
value through unchanged.  In a three-source MAD, source-language operands
one, two, and three occupy native A (`dword1[27:24]`), C
(`dword0[31:28]`), and B (`dword0[3:0]`).  Windows additionally sets dword-3
bit 20 when source C is NEG:

```text
Windows: 80820530 30081053 00f08005 00100020
Linux:   80820530 30081053 00f08005 00000020
```

Thus the Linux mapping over D3D modifier enum values 0..13 is
`0,8,2,3,4,5,6,7,8,9,a,b,c,d`.  The Windows programmable converter instead
maps the common semantic set NONE, NEG, BIAS, BIASNEG, SIGN, SIGNNEG, COMP,
X2, and X2NEG to native `0,8,1,9,5,d,2,4,c`.  DZ, DW, ABS, ABSNEG, and NOT
have no cross-driver semantic proof.

This differs from both the Linux encoding and an earlier interpretation of
the Windows differentials as four composable semantic bits.  Until numerical
hardware tests resolve the version/chip discrepancy, only NONE and NEG
should be emitted by new code; BIAS/SIGN/COMP/X2 remain compiler-oracle-only.

Pixel destination-modifier validation has the same parity bug as the vertex
path.  `stmPPS_GetALUDestMod_invi @0x161c70` indexes a 16-entry table; a full
raw-token sweep shows every odd modifier code sets native dword-3 saturate bit
12 and every even code clears it.  Only NONE and SAT are semantic.  Reject all
other destination modifiers rather than accepting their silent parity alias.

The first texture differentials establish useful pre-post-translation TAU
golden vectors:

```text
texld  r0,t0,s0: 201001b0
texldp r0,t0,s0: 207001b0
texldb r0,t0,s0: 203001b0
```

Sampler identifiers are duplicated in bits `27:24` and `3:0`: changing s0 to
s1 produces `211001b1`.  Static emitters plus executable differentials assign
the complete TAU word used by these operations:

| Bits | Meaning |
|---:|---|
| `31:29` | fixed class bits `001` |
| `28` | dependency/presence bit; clear in the raw emitter, set by the target finalizer |
| `27:24` | sampler id |
| `23:20` | operation: 1 TEXLD, 3 TEXLDB, 4 TEXKILL, 7 TEXLDP |
| `19:16` | allocated temporary destination id |
| `17:12` | allocated texture-coordinate/source id |
| `11:10`, `9:8`, `7:6`, `5:4` | X, Y, Z, W selectors |
| `3:0` | duplicated sampler id |

The destination and source contributions deliberately overlap at bits 17:16:
the proprietary emitter ORs `(dst_id & 0xf) << 16` with
`(src_id & 0x3f) << 12`, and its register allocator supplies compatible
physical ids.  They must not be encoded as two independently replaceable
bitfields after allocation.

The proprietary PS 2.0 frontend accepts sampler tokens `s0..s15` and its raw
emitter produces the exact sequence
`201001b0,211001b1,...,2f1001bf` for otherwise identical TEXLD operations.
Configuration-2 post-translation additionally sets bit 28 on an active
record; for example the one-sampler execution word is `301001b0`.  One shader
containing all 16 distinct sampler tokens also
compiles successfully into 16 TAU records followed by one ALU output; its
T2A map is exactly bit 15 (`object+0x60=00008000`).  Thus the native operand
and scheduler intentionally cover a real four-bit sampler namespace, even
though both supplied OpenGL ICDs advertise only eight texture units and only
stage zero has live silicon proof.  Worse, the frontend does not validate
indices above 15: it ORs the unchecked index both at bit 24 and into the low
word.  For example s16, s32, and s255 produce `301001b0`, `201001b0`, and
`ff1001ff`, respectively, corrupting dependency/class and/or swizzle fields.
The raw invalid-s16 word is numerically identical to a correctly finalized
s0 word because both set bit 28 for unrelated reasons.  Validate the logical
sampler id before encoding; a native-word filter cannot distinguish them.
Mesa must range-check sampler ids before encoding; expose only s0..s7 until
higher-stage hardware tests justify raising the API cap.

Identity swizzle is `0x1b0`; replicated xxxx, yyyy, zzzz, and wwww are
`0x000`, `0x550`, `0xaa0`, and `0xff0`.  Texture-coordinate and temporary ids
are post-allocation native ids, not necessarily the API numbers.  `texkill
r0` emits ALU `01c00530 301c0053 00f00005 000d0420` and TAU `214001b1`; it
uses the special scratch allocation path rather than an ordinary sampler.
Texture programs also require `stmPPS_PostTranslationSetting_invi`,
sampler-format state, and the texture cache/state packets; a TAU word alone is
not a safe texture test.

### Per-bit unknowns and execution hazards

For vertex operands, negate, four three-bit swizzle/literal selectors, and the
unified source index are assigned; source bits 1 and 23..31 remain unresolved.
Vertex destination
mask, index, saturation, primary selector, and relative selector are assigned.
Bit 11 is set by final oPos handling (`00080a0f`) but not by the bare MOV
emitter (`0008020f`), so it remains a finalizer-owned bit.  Supported VS flow
targets/extents occupy dword 1 or dword 2 bits `18:10` as detailed above;
their runtime direction, delay, and nesting behavior remain untested.

For pixel ALU words, the semantic fields in the table above are assigned.
Unassigned bits include dependency flags (`dword0` bit 25, `dword1` bit 21,
`dword2` bit 17, `dword3` bits 28/29), dword-3 low dependency slots, and
the remaining reserved fields.  They are modified by scheduling and must
not be zeroed or synthesized after finalization.  The PS 2.0 dispatcher has no
CALL, IF, or LOOP entries: its ALU/TAU program is straight-line.  PS 1.x
coissue/context-switch machinery must not be mistaken for general PS 2.0
predication or branching, and no predicate field is proven.  The compiler
limits and valid encodings do not establish latency, scoreboarding, branch
delay behavior, cache visibility, or fault recovery; those require fenced
hardware experiments.

### Historical vertex-stall signature and resolution

A live VX900 draw attempt left MMIO `0400 = 00080031`.  The low bits are
bit 0 (overall 3D busy), bit 4 (command regulator busy), and bit 5.  The HC3
manual's general engine-status table names bit 5 `HRVPSt`, vertex processor
busy; the old KMD independently includes bits 0 and 5 in its `00001fe1` 3D
busy mask.  Pixel-shader bit 10 and all setup/clip/primitive/pixel downstream
bits were clear.  This localizes the hang to vertex/front-end state, not pixel
constants, render-target output mapping, or PS execution.  Bit 19 (`00080000`)
is reserved in the HC3 table and remains unassigned on VX900.

This failure is now resolved: the attempted 160-dword body was not the complete
vendor emission.  The corrected exact 188-dword body, combined with the proven
initialization and cache-flushed two-phase CR prime, retired at pause
`000004f0` with CR `c0000000`, engine/error zero, all 1024 target pixels
`000000ff`, intact guards, and successful state restoration.  The measured
module SHA-256 is
`71290715146fa8a0565870d422e12d1e14d5e6366821f3553835b37714eb86b9`.
See [vp-stall-analysis.md](vp-stall-analysis.md) for the exact causal proof.

The following upload-only gates were the proposed discriminator before the
complete body was recovered.  They remain useful future diagnostics, but are
not prerequisites for bring-up and must not replace the verified 188-dword
oracle.  Each stream should append the already-proven status packet and CR
pause, then require `0400 & 00001ff3 == 0` and a successful guarded 2D fill:

```text
# PS store only
fe020440 0000043c 20030000 cc000000
01c00000 301c0000 00f04005 000d083f

# VS store only, one NOP
fe020440 0000043c 31030000 cc000000
00000000 00000000 00000000 00000000
```

1. Upload only the Xorg PS instruction under selector `20030000`; do not emit
   vertex data or FVF/VS controls.
2. Upload one all-zero VS NOP under selector `31030000`; do not emit
   `60800003`.
3. Upload the four Xorg H6 VS instructions listed below, still omitting
   `60800003` and vertex data.
4. Add the PS `90`-`99` control block without any vertex/fire packet.
5. Add `60800003` by itself as a separate no-vertex gate.  If this alone
   asserts VP busy, it must be sent atomically with the exact FVF setup.

No recovery-register experiment should be combined with these gates.  Log
`0400`, `0444`, `043c`, `045c`, `0460`, and `0464` on every timeout.

## Minimal no-texture shader packets

The packet-exact execution oracles, including a six-dword constant-color
derivative of the silicon-proven 188-dword draw and the complete proven
texture-sample linkage, are consolidated in
[shader-upload-oracles.md](shader-upload-oracles.md).

The smallest compiler-derived constant-color program is
`ps_2_0; mov oC0,c0; end`, compiled independently by both drivers:

```text
fe020440 0000043c 00010000 cc000000
90058000 91000000 92000001 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000

fe03043c 00000004 20030000 cc000000
01c00530 301c0053 00f08005 000d0820

# c0 = opaque red
fe03043c 00000004 22030000 cc000000
3f800000 00000000 00000000 3f800000
```

`90058000` is the configuration-2 execution form.  The compile pass alone
reports `90040000`; that pre-post-translation value is not an upload oracle.

For `vs_2_0; mov oPos,v0; end`, the fully dispatched position instruction is
`0008014c 00000000 00000000 00080a0f`, but the zero-context probe cannot
supply safe complete VS control state.  The exact Xorg no-texture H6 program
is four instructions followed by last-index/control word `60800003`:

```text
00380150 00000000 00000000 0008010f
0008014c 00100150 00384154 0004021f
0008414c 00000000 00000000 0008422f
0008814c 00000000 00000000 0008423f
60800003
```

This comes directly from `via3DEmitVetexShader_H6` at
`via_drv.so:0x30a60` (`0x30a7d`-`0x3113b`).  It is coupled to Xorg's
pretransformed position/diffuse/specular vertex layout and is not interchangeable
with a generic MOV-only shader.

These are shader packets, not a complete draw.  Render-target addresses and
pitch/format, viewport, varying linkage, vertex fetch, primitive setup,
flush/invalidate, draw, fence, and readback state must be programmed by the
bring-up path.  The five viewport/position fixups generated with the probe's
synthetic zero context are deliberately excluded: their constants depend on
real context state.

For the very first VX900 execution, prefer the complete Xorg
`Modulate_H5_No_Tex` control/program vector in
[`via-5.75-dri-state.md`](via-5.75-dri-state.md): PCI `7122` explicitly selects
that H5 pixel path through the H6 initializer.  The compiler-derived stream
above assumes zero runtime control state and is better used as the next
programmable-constant test after the opaque Xorg vector succeeds.

## How to extend the offline oracle

The immediate high-value probes are:

1. Feed `chrome9_vs_compile_probe` one-token differentials for remaining VS
   2.0 declarations and structured flow combinations.  Save raw tokens plus
   all four native dwords as a corpus.
2. Add a decoder for the established vertex source/destination fields and
   assert round trips against that corpus.
3. Use `CHROME9_POSTPROCESS=2 chrome9_pps_probe` for every candidate execution
   vector and compare it with the unfinalized default run.  Extend that corpus
   from the proved one-TAU dependency bits to two-TAU, TEXLDP/TEXLDB, and
   TEXKILL programs before enabling those shapes.
4. Sweep the Linux PPS and Wine/Windows oracles instruction-for-instruction.
   Agreement across two independently built proprietary drivers is the
   strongest available static/dynamic check.
5. Do not execute branch, texture, or control-flow samples on hardware until
   they decode consistently in both binaries.  Start hardware execution with
   a MOV-only VS, MOV-only PS, no texture, one triangle, and a fenced readback.

## Confidence summary

- **Exact code evidence:** packet selectors, element widths, object offsets,
  96/32 pixel limits, the twelve PS control-word sources, default pixel words.
- **Executable compiler evidence:** VS parser/dispatcher invocation, native
  instruction width, source swizzles/modifier/relative selector, unified
  register maps, destination mask/index/saturate, primary vertex selectors and
  listed lowerings; complete Linux PS 2.0 tokenizer/dispatcher invocation,
  register limits, TAU layout, and cross-driver MOV match.
- **Still incomplete:** vertex flow-control runtime/delay behavior, final
  dependency/control bits, executable texture state beyond the TAU word,
  scheduling hazards, and numerical behavior on silicon.
