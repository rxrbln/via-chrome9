# Chrome9 HD shader ISA recovery

Status: partial static decode.  This document records facts recovered directly
from the VX900 Windows OpenGL ICD and Direct3D UMD.  Analysis addresses apply
only to the binaries and hashes listed in [windows-driver-re.md](windows-driver-re.md).
Names such as `operand A` are deliberately neutral until instruction semantics
are proven.

## Program-upload packet

Both userspace drivers emit programmable state with a type-03 packet:

```text
optional cc000000 padding until the packet is 16-byte aligned
fe03043c
payload_dword_count
(subtype << 24) | (03 << 16) | (parameter_offset << 8) | parameter_address
cc000000
payload_dword[0 ... count-1]
```

The lower selector bytes are the manual's `HParaOS:HParaAdr` tuple, not a
single flat address.  The KMD decoder at `0x000a0db0` writes the selector to
MMIO `043c`, skips the dummy word, and streams the counted payload through
MMIO `0440`-`044c` in groups of at most four dwords.  This independently
confirms the userspace packet interpretation.

Recovered program selectors and widths:

| Selector/subtype | Payload | Statically observed meaning |
|---:|---:|---|
| `20030000` | `instruction_count * 4` dwords | pixel ALU program; each instruction is 4 dwords / 16 bytes |
| `21030000` | `instruction_count` dwords | pixel texture-address program; each instruction is 1 dword |
| `22030000`, `22030040`, `22030080`, `220300c0` | bank-dependent | pixel float-constant banks beginning at dword 0, 64, 128, and 192 |
| `31030000` | `instruction_count * 4` dwords | vertex program; each instruction is 4 dwords / 16 bytes |
| `3203xxxx` | run length times 4 dwords | vertex float constants; low 16 bits select the `HParaOS:HParaAdr` start |
| `33030000` | 16 dwords | 16 packed vertex integer constants, four byte components per dword |

The full vertex-float-constant path emits selector `32030400`, count `0400`,
and copies `1000` bytes: 256 `vec4` constants.  A dirty-run path changes the
selector's lower 16 bits by four dwords per constant and transfers only
contiguous dirty vectors.

Pixel float constants use four banks.  Banks 0 and 2 transfer 64 dwords;
banks 1 and 3 transfer 28 dwords.  Narrow updates to selectors `2203005c` and
`220300dc` transfer the final four dwords in the shorter banks.  These are
observed software limits; their relationship to physical store capacity still
needs proof.

Immediately before pixel-program upload, the ICD emits a type-02 stream that
starts with:

```text
fe020440 0000043c 00010000 cc000000
```

It then emits twelve control words whose high bytes run from `90` through
`9b`.  `92` contains ALU count in the low half and TAU count in the high half.
`90.bit18` selects an ALU-first program, `93/94` are the TAU-to-ALU transition
bitmap, and `95..9a` are the ALU-to-TAU transition bitmap.  Bitmap words carry
24 instruction positions apiece; a set bit changes stores after the indexed
instruction.  Sources for all twelve control words and the remaining opaque
fields are detailed in [via-5.75-shader-re.md](via-5.75-shader-re.md).

## Shared native compiler

The OpenGL ICD constructs a `05ac`-byte compiler/cache object at
`0x102463c0`; the exact function byte sequence occurs in the Direct3D UMD at
`0x101ddc20`.  Two other substantial backend routines are also byte-identical:

| Role | OpenGL ICD | Direct3D UMD |
|---|---:|---:|
| compiler/cache object construction | `0x102463c0` | `0x101ddc20` |
| scheduling and stream rewrite | `0x10256550` | `0x10203530` |
| instruction metadata finalization | `0x10254d30` | `0x10201cb0` |

This is direct evidence of a common native backend, rather than an inference
from registry strings.  It also gives two independently reachable source
frontends—ARB programs and Direct3D shader bytecode—for recovering the same
machine encoding without running Windows.

The compiled pixel-program view begins 16 bytes into the object:

| View offset | Meaning |
|---:|---|
| `04` | pointer to the ALU instruction vector |
| `08` | ALU instruction count |
| `10` | pointer to the texture-address instruction vector |
| `14` | texture-address instruction count |

The ALU vector has 16-byte elements and is initially allocated for 32 entries.
The texture vector has 4-byte elements and is initially allocated for 11.
These values are only allocation capacities.  The append routines at
`0x10253760` and `0x10253810` enforce maxima of 96 ALU and 32 texture
instructions respectively, exactly matching the HC3/HCM program-store sizes.
The optimizer builds 20-byte combined scheduling records, rewrites both output
vectors, and updates both counts.  Its separate 128-entry combined-schedule
bound is therefore the sum of the two store limits, not an additional store.

The OpenGL compilation path around `0x102469a0` calls frontend/setup routines,
then the passes at `0x10258120`, `0x10256550`, `0x10258ee0`, optional passes at
`0x10258200` and `0x10257b70`, and the finalizer at `0x10255740`.  This call
chain is the current roadmap for assigning instruction semantics.

## Direct3D vertex compiler and offline oracle

The Direct3D UMD has a separate, older native vertex compiler which is a close
binary match for the symbolized Linux compiler.  The VS 2.0 table selected for
the H5/H6 chip entry is at `0x103367f8` and contains these four functions in
the exact order called by `0x10207180`:

| Table offset | Windows VA | Linux analogue | Role |
|---:|---:|---|---|
| `00` | `0x102407b0` | `PVSCP_MSVSPARSER_VS20` | D3D token parser |
| `04` | `0x102415f0` | `PVSCP_VALIDATE_VS20_H5` | constants/input remap |
| `08` | `0x1023f4b0` | `PVSCP_DISPATCHER_VS20_H5` | native instruction emission |
| `0c` | `0x10241680` | `PVSCP_POSTPROCESS_H5` | flow-target fixups |

The call sequence at `0x10207519..0x1020753b` is parser, validator, install
the native-object pointer at parsed-object offset `39fc`, dispatcher, then
postprocess.  The parser's first argument is a zeroed `3a0c`-byte object; its
second argument is `{unused, byte_size, token_pointer}`.  The native object
stores its instruction count at `+424` and consecutive 16-byte records at
`+42c`.  The normal upload path at `0x1015abb0` reads exactly those fields and
emits selector `31030000` at `0x1015af74` with `count * 4` payload dwords.

The validator does not provide a safety boundary.  Its immediate-integer
constant path reads a scalar count at parsed `+36ac`, 16-bit values from
four-byte slots beginning at `+32ac`, converts each to float, and stages them
in the driver constant cache.  It marks dirty bits `0xc00` and always calls
the input-remap routine at `0x102413f0`; there are no opcode, length, branch,
or native-bound checks.  Float and integer/boolean hardware uploads remain
separate from compilation and use the `3203xxxx`/`33030000` packets described
above.

[`../../tools/wine_vs_shader_probe.c`](../../tools/wine_vs_shader_probe.c)
reproduces this private four-stage call chain under Wine without opening an
adapter or calling the KMD.  The dispatcher requires one software-only chip
descriptor reached through wrapper `+0c`, driver view `+9c3c`, descriptor
`+1c`; the H5/H6 value used by the normal path is `8`.  Omitting only that
pointer lets the complete dispatcher run and then fault at
`0x1023fdfd`, which independently identifies the field.  Supplying it makes
parser, validator, dispatcher, and postprocess all return zero.

The built-in VS 2.0 probe defines `c0=(1,2,3,4)`, executes
`add r0,v0,c0; mov oPos,r0`, and writes both the normalized `3a0c`-byte object
and bounded native object to `/tmp`.  The compiled object contains nine native
records (`+424 = 9`) and is `0x4bc` bytes through the last record.  This gives
an offline differential oracle for VS opcodes, operands, flow fixups, and
compiler metadata; it does not establish execution behavior or make the
vendor compiler safe for untrusted bytecode.

## Direct3D semantic-to-emitter map

The Direct3D frontend provides a static semantic oracle.  The compiler at
`0x101de220` recognizes pixel-shader version tokens `ffff0100` through
`ffff0104` and `ffff0200`.  Its shader-model-2 tokenizer at `0x101dbd30` uses
the standard `D3DSIO_*` opcode values and normalizes instructions into
36-byte internal records, with a checked maximum of 128 records.  This is an
intermediate-representation limit and must not be confused with the 96-entry
hardware ALU store.

The first token is also tested as a signed value: a normal `ffffxxxx` D3D
shader-version token sets object field `5a8`, selecting the programmable
dispatcher at `0x101dca80`.  A second dispatcher at `0x101dc6a0` is used for
the non-programmable/internal path.  Most operations share an emitter; the
table shows both where they differ:

| D3DSIO | Operation | Programmable emitter | Other/internal emitter |
|---:|---|---:|---:|
| `01` | MOV | `0x1021ab10` | `0x10218b70` |
| `02` | ADD | `0x1021a5f0` | `0x102157b0` |
| `03` | SUB | `0x1021add0` | `0x1021a1b0` |
| `04` | MAD | `0x1021a950` | `0x102185f0` |
| `05` | MUL | `0x1021ac50` | `0x10218f10` |
| `06` | RCP | `0x10219920` | same |
| `07` | RSQ | `0x10219a80` | same |
| `08` | DP3 | `0x1021af60` | `0x10216040` |
| `09` | DP4 | `0x10216210` | same |
| `0a` | MIN | `0x102189b0` | same |
| `0b` | MAX | `0x102187f0` | same |
| `0d` | SGE | `0x10219700` | same |
| `0e` | EXP | `0x10216740` | same |
| `0f` | LOG | `0x10216f00` | same |
| `11` | DST | `0x10219550` | same |
| `12` | LRP | `0x1021a770` | `0x10217060` |
| `13` | FRC | `0x102168a0` | same |
| `14` | M4x4 | `0x10218180` | same |
| `15` | M4x3 | `0x10217e00` | same |
| `16` | M3x4 | `0x10217980` | same |
| `17` | M3x3 | `0x102175f0` | same |
| `18` | M3x2 | `0x10217350` | same |
| `20` | POW | `0x10219320` | same |
| `21` | CRS | `0x10215b70` | same |
| `23` | ABS | `0x10215590` | same |
| `24` | NRM | `0x102190c0` | same |
| `25` | SINCOS | `0x10219f20` | same |
| `41` | TEXKILL | `0x1021c1d0` | same |
| `42` | TEX | `0x1021bae0`, `0x1021bd50`, or `0x1021bf90` according to token flags | same |
| `58` | CMP | `0x10215970` | same |
| `5a` | DP2ADD | `0x1021b710` | `0x10215d60` |

Opcode `0c` (SLT) has no direct call in these dispatchers because it is lowered
to internal opcode 107.  The previously opaque internal range is now assigned
by combining the Windows jump table with the Linux DSO's `_OPCODE_tag` DWARF
and symbolized dispatcher:

| Internal opcode | Linux name / role | Windows programmable handler | Emitter |
|---:|---|---:|---:|
| 102 | `STM_DPH_ICD` | `0x101dcb1c` | `0x102163b0` |
| 103 | `STM_FLR_ICD` | `0x101dcb60` | `0x10216a10` |
| 104 | `STM_SWZ_ICD` | `0x101dcb71` | `0x10216c20` |
| 105 | split SINCOS component, mode 1 | `0x101dccb4` | `0x10219be0(...,1)` |
| 106 | split SINCOS component, mode 0 | `0x101dccc7` | `0x10219be0(...,0)` |
| 107 | `STM_SLT_ICD` | `0x101dcb2d` | `0x10216550` |

Opcodes 105 and 106 are deliberately not labelled SIN versus COS yet.  Linux
`stmPPSDispatcherPS20_invi` proves that both call
`stmPPS_SIN_COS_ICD_PS20_invi` with the final argument one or zero, while the
Windows emitter uses that argument to select an extra `0x249` immediate.  The
component-to-mode direction still needs a controlled frontend differential.

The programmable MOV wrapper delegates an unmodified MOV
to `0x10218b70`; modifiers take a separate lowering path.  This makes MOV the
best first routine for attaching semantic names to the raw fields below.

## Bounded first pixel-compiler subset

The smallest useful replacement for the current fixed-shader classifier does
not need the vendor optimizer or arbitrary texture scheduling.  A bounded
straight-line compiler can support two program shapes while using only proven
object/control transitions:

1. zero or more ALU operations ending in `MOV oC0,...`;
2. exactly one leading `TEXLD rN,t0,s0`, followed only by ALU operations and
   ending in `MOV/MUL/... oC0,...`.

For an ALU-only program with `N` native instructions, the compiler-derived
control tuple is:

```text
90040000 91000000 920000NN 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

For one leading texture instruction followed by `N` ALU instructions, it is:

```text
90000000 91000000 920100NN 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

Here `NN` is the ALU count in the low 16 bits, not an eight-bit textual
substitution.  The second tuple starts in TAU and switches to ALU after TAU
index zero; it never switches back.  Both tuples precede ordinary type-03
uploads: `N*4` dwords under `20030000`, then zero or one dword under
`21030000`.  Runtime integration must preserve the normal H2 packet builder
and any nonzero `9b` low state bits rather than blindly copying these zero-
context examples.

The exact one-texture MODULATE body produced by both offline compilers is:

```text
ALU 0140b6d0 30040053 00f00005 00000820
TAU 201001b0
```

The corresponding D3D tokens are `texld r0,t0,s0; mul oC0,r0,v0`.  This is a
general compiler boundary rather than another fixed shader only if operands,
writemasks, swizzles, constants, and temporary allocation are encoded from IR
and independently range-checked.

The first implementation should enforce all of these bounds before emission:

| Resource/feature | Initial truthful bound |
|---|---|
| native ALU records | at most 96 after lowering |
| native TAU records | zero or one initially; hardware/compiler store bound 32 |
| temporaries | ids 0..11; reserve 12..15 for later lowering/spill support |
| interpolated inputs | only linkage already exercised by fixed paths (`v0`, and `t0` for TAU) |
| float constants | `c0..c15` initially through `22030000`; architectural compiler map extends to `c31` through `22030080` |
| samplers | only `s0` initially; native field is four bits and compiler accepts `s0..s15`, but API proof currently covers fewer |
| outputs | only `oC0`; native color ids 0..3 and depth id 4 exist but are not part of this first subset |
| control flow / indirection | none |
| source modifiers | NONE and NEG only |
| destination modifiers | NONE and SAT only |

The safest first ALU allowlist is the one-native-record family `MOV`, `ADD`,
`SUB`, `MUL`, and `MAD`, plus literal zero/one and normal XYZW swizzles.  Their
exact templates and per-field differentials are in
[via-5.75-shader-corpus.tsv](via-5.75-shader-corpus.tsv).  DP3/DP4,
MIN/MAX/CMP, reciprocal/transcendental operations, multi-record lowerings,
discard, depth output, dependent texture reads, and additional samplers can be
enabled independently after numerical and scheduling tests.  The vendor
compiler's successful return is not validation: it silently truncates ALU and
TAU stores and accepts several invalid register/modifier encodings.

## Direct3D operand normalization

The Direct3D token converters at `0x1020f070`/`0x1021a400` (source) and
`0x1020f200` (destination) provide semantic labels for several native fields.
They first decode the split D3D register-type field
`(((token >> 20) & 0x0700) | (token & 0x1800)) >> 8` into a compact internal
operand.  The source register-class conversion at `0x1020e910` is:

| D3D register type | D3D meaning in a pixel shader | Internal source class |
|---:|---|---:|
| 0 | TEMP | 0 |
| 1 | INPUT | 1 |
| 2 | CONST | 2 |
| 3 | TEXTURE/ADDR | 3 |
| 4-16 | other register files | 0 |
| 17 | MISCTYPE | 3, with identifier forced to 15 |

TEMP and TEXTURE identifiers pass through allocation callbacks at object
vtable offsets `139c` and `1398`; other identifiers normally come directly
from D3D token bits `10:0`.  Consequently these class numbers describe the
compiler's compact operand and the native source-class field, but do not prove
that every D3D register file listed in the catch-all range is legal in every
ALU operation.

The destination conversion at `0x1020e800` maps TEMP to class 0,
TEXTURE/ADDR to class 1, and the remaining accepted destination types to class
2.  It packs the four-bit D3D writemask, the result-saturation bit, and the
legacy result scale.  Scale codes 1, 2, and 3 mean multiply by 2, 4, and 8;
D3D codes 15, 14, and 13 (divide by 2, 4, and 8) become native codes 5, 6,
and 7.  Code 0 remains 0.

For a normal (non-replicated) D3D source swizzle, the converter places the
four selectors into three-bit native fields.  Tracing that layout through the
ADD emitter at `0x1021a5f0` proves the following native fields.
The names `source A`, `source B`, and `source C` describe encoding slots, not
evaluation order.  Source C was established with three-source MAD
differentials through the Wine oracle:

| Native field | Meaning |
|---|---|
| dword 0 `3:0` | source B modifier bitset: bias, complement, x2, negate |
| dword 0 `15:13`, `12:10`, `9:7`, `6:4` | source B X, Y, Z, W three-bit swizzle/literal selectors |
| dword 0 `21:16` | source B six-bit identifier |
| dword 0 `24:22` | source B three-bit register class |
| dword 0 `31:28` | source C modifier bitset: bias, complement, x2, negate |
| dword 1 `27:24` | source A modifier bitset: bias, complement, x2, negate |
| dword 1 `31`, dword 2 `1:0` | source A split Z three-bit selector |
| dword 1 `30:28` | source A W three-bit selector |
| dword 2 `4:2` | source A Y three-bit selector |
| dword 2 `7:5` | source A X three-bit selector |
| dword 2 `13:8` | source A six-bit identifier |
| dword 2 `16:14` | source A three-bit register class |
| dword 1 `20:18` | source C three-bit register class |
| dword 1 `17:12` | source C six-bit identifier |
| dword 1 `11:9`, `8:6`, `5:3`, `2:0` | source C X, Y, Z, W three-bit swizzle/literal selectors |
| dword 2 `23:20` | destination XYZW writemask |
| dword 3 `9:6` | destination four-bit identifier |
| dword 3 `11:10` | destination two-bit register class |
| dword 3 bit 12 | saturate/clamp result |
| dword 3 `15:13` | legacy result scale code |
| dword 3 bit 20 | source-C-modifier companion/presence flag |

The split source-A selector is not a transcription error: its Z field crosses
the dword-1/dword-2 boundary.  Values 0, 1, 2, and 3 mean X, Y, Z, and W.
Native values 4 and 5 mean literal one and zero: ADD's `30140924` has four
source-C selector-4 fields and MUL's `0140b6d0` has four source-B selector-5
fields.  Both literal slots also carry source register class 5, so decoders
can label class 5 as literal/special; the selector remains the value-bearing
field.  Values 6 and 7 remain unassigned.  What previously looked like a gap
between two-bit D3D swizzles is the literal-selector high bit.

The programmable source-modifier converter at `0x1021a370` makes the native
four-bit modifier a composable bitset.  The assignments are bit 0 = BIAS
(subtract 0.5), bit 1 = COMP (`1-x`), bit 2 = X2, and bit 3 = NEG.  The D3D
mapping is therefore NONE=0, NEG=8, BIAS=1, BIASNEG=9, SIGN=5,
SIGNNEG=13, COMP=2, X2=4, and X2NEG=12.  This equivalence is also a useful
test oracle for a future encoder.

Wine differentials independently confirm source C's modifier nibble: BIAS,
COMP, X2, and NEG produce dword-0 bits 28, 29, 30, and 31 respectively, while
each nonzero modifier also sets dword-3 bit 20.  Varying only the middle D3D
MAD operand across TEMP, INPUT, CONST, and TEXTURE produces source-C class
values 0, 1, 2, and 3 in dword-1 bits `20:18`.  Its identity swizzle is
`0x053`; changing individual selectors proves the sparse `10:9`, `7:6`,
`4:3`, `1:0` layout.

## Partial pixel ALU encoding

The dependency/finalization passes repeatedly extract the following fields
from each 128-bit ALU instruction.  Bit numbering is within the named dword.
These are encoding facts.  The D3D ADD path supplies the source/destination
labels shown here; fields not reached by that path retain neutral names.

| Dword | Bits/mask | Observed use |
|---:|---:|---|
| 0 | `21:16` (`003f0000`) | source B 6-bit register identifier |
| 0 | `24:22` (`01c00000`) | source B 3-bit register-class selector |
| 0 | bit 25 (`02000000`) | flag associated with that identifier |
| 0 | `31:28` (`f0000000`) | source C modifier bitset |
| 1 | `17:12` (`0003f000`) | source C 6-bit register identifier |
| 1 | `20:18` (`001c0000`) | source C 3-bit register-class selector |
| 1 | bit 21 (`00200000`) | flag associated with source C identifier |
| 1 | `11:9`, `8:6`, `5:3`, `2:0` | source C X, Y, Z, W three-bit selectors |
| 2 | `13:8` (`00003f00`) | source A 6-bit register identifier |
| 2 | `16:14` (`0001c000`) | source A 3-bit register-class selector |
| 2 | bit 17 (`00020000`) | flag associated with that identifier |
| 2 | `23:20` (`00f00000`) | destination XYZW writemask |
| 3 | `9:6` (`000003c0`) | destination 4-bit identifier |
| 3 | bit 29 (`20000000`) | flag associated with the `9:6` identifier |
| 3 | `3:0` (`0000000f`) | second 4-bit identifier |
| 3 | bit 28 (`10000000`) | flag associated with the `3:0` identifier |
| 3 | `11:10` (`00000c00`) | destination 2-bit register-class selector |
| 3 | bit 12 (`00001000`) | destination saturation/clamp |
| 3 | `15:13` (`0000e000`) | destination result-scale code |
| 3 | `5:4` (`00000030`) | second 2-bit register-class selector |
| 3 | `19:16` (`000f0000`) | one ALU sub-operation selector; not a complete instruction opcode |
| 3 | bit 20 (`00100000`) | companion/presence flag for a source C modifier |

For each 2-bit class, values 0-2 use identifiers 0-15.  Class 3 remaps the
logical range to identifiers 12-27.  The optimizer treats identifier ranges
`0c`-`0f` and `20`-`23` specially while constructing dependencies.  Assigning
architectural register-file names to those ranges would currently be
speculation.

The Direct3D semantic map makes part of the dword-3 selector enumerable.  An
emitter can use the same selector for multiple API operations because other
dwords select parallel ALU functions or modifiers, so these values are not a
standalone opcode table:

| Dword-3 `19:16` | Operations whose emitters use it |
|---:|---|
| `0` | ADD, MAD, MUL, and SUB paths |
| `2` | DP3 |
| `3` | DP4 |
| `6` | RCP |
| `7` | RSQ |
| `9` | ABS and CMP paths |
| `b` | MAX |
| `c` | MIN |
| `d` | MOV |

The texture-word finalizer also extracts bits `23:21`; value 2 sets a compiled
program flag at view offset `6c`.  The flag's hardware meaning is unknown.

## Cross-version validator boundary

Executable Linux 5.75 differentials add limits which the Windows static map
does not enforce.  These belong in the Mesa validator, before packet emission:

- PS native stores hold exactly 96 ALU and 32 TAU records, but vendor compile
  still returns success when it silently truncates record 97 or TAU 33; its
  128-record tokenizer crashes on record 129.
- VS float constants stop at `c255`; invalid `c256` wraps computed unified
  index 512 to register zero.  `i16` aliases `b0` and `b16` aliases `i0`.
- PS `c32..c47` collide with native internal/gap ids 48..63 and `c48..c63`
  wrap to native ids 0..15.
- Native TEXLD intentionally carries sampler ids 0..15, but an unchecked s16
  corrupts the fixed high class (`301001b0`), s32 aliases s0, and s255 also
  corrupts low swizzle bits (`ff1001ff`).
- VS `oT8` wraps outside the varying space and PS `oC4` aliases the native
  depth destination without the depth-present flag.  Valid architectural
  ranges are `oT0..oT7` and native `oC0..oC3`; current GL exposure remains one
  render target.
- Predication/coissue flags are ignored, PS relative constants are rejected,
  and BREAK/BREAKC/IFC can return success without a native operation.

The replayable Linux corpus and complete VS/native field map are maintained in
[via-5.75-shader-re.md](via-5.75-shader-re.md) and
[via-5.75-shader-corpus.tsv](via-5.75-shader-corpus.tsv).

## Confidence and remaining work

The packet shapes, selectors, element widths, compiler-object layout, physical
96/32 program limits, D3D semantic dispatch, and listed masks/shifts are
**Code** evidence.  Basic ALU source identifiers/classes, swizzles,
destination identifier/class, writemask, saturation, and result scale are now
mapped.  Vertex source/destination fields, primary selectors, relative-address
selectors, structured-flow fixups, and compiler expansions are now decoded in
the Linux cross-reference above.  Pixel dependency-gap semantics, branch
runtime/delay behavior, texture-format state, and numerical corner cases
remain incomplete.

The next static steps are:

1. label every read and write of the two output vectors in both binaries;
2. extract all masks, shifts, and enum comparisons from the finalizers into a
   machine-readable field table;
3. trace the chip-selected callback tables installed around ICD addresses
   `0x10245fc0`-`0x10246335` and identify the VX900 implementation;
4. map ARB and Direct3D frontend operations to the shared backend's internal
   opcodes, using byte-identical routines as alignment anchors;
5. write a standalone decoder before writing an encoder, and require every
   claimed semantic field to have at least two independent static witnesses;
6. execute only validator-approved straight-line vectors first; retain the
   recovered `90`-`9b` transition tuple rather than regenerating dependency
   bits after scheduling.

Hardware execution will eventually be required to prove cache visibility,
fault behavior, numerical corner cases, and scheduling hazards.  It is not a
prerequisite for continuing the static ISA extraction.

The current field map is executable in
[`../../tools/chrome9_alu_decode.py`](../../tools/chrome9_alu_decode.py).  It
accepts four integer dwords and emits JSON; it deliberately reports unresolved
dependency slots rather than assigning speculative semantics.

The private backend is also executable under Wine through
[`../../tools/wine_shader_probe.c`](../../tools/wine_shader_probe.c).  Its
built-in `ps_2_0` MOV sample dynamically produced
`01c00530 301c0053 00f08005 000d0020`, matching the static field map.  The
probe can consume another D3D token stream from `/tmp/chrome9-shader.bin` and
emit both raw and post-scheduler/finalizer ALU/TAU vectors without Windows,
the VIA KMD, or GPU hardware.  The one-instruction MOV and MAD samples are
unchanged by scheduling, as expected.  Real execution remains a separate
evidence level.
