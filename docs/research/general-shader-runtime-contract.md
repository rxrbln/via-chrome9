# Chrome9 general shader runtime contract

This note turns the recovered Chrome9 HD shader ISA into the smallest
implementation path beyond the current five runtime fragment classes. It is
not permission to submit arbitrary native words. The first useful target is
glamor's solid/copy/composite family while Gallium `draw` continues to execute
the vertex shader in software. Native clip-space VS execution is a later,
independently gated optimization.

The current runtime recognizes primary color, constant color, one-texture
REPLACE, one-texture MODULATE, and two-texture MODULATE. The checked offline
compiler already produces fifteen exact, finalized fragment forms and eight
vertex templates. Its standalone test currently reports:

```text
Chrome9 offline shader corpus: 15 PS + 8 VS templates and verifier passed
```

The immediate implementation gap is therefore state transport, validation,
constant upload, and linkage—not recovery of another opaque shader word.

## Provenance and confidence

The two 2015 Windows UMDs share the native shader backend. Exact duplicate
routine bodies occur at:

| role | Direct3D UMD | OpenGL ICD |
|---|---:|---:|
| compiler/cache construction | `0x101ddc20` | `0x102463c0` |
| scheduling and stream rewrite | `0x10203530` | `0x10256550` |
| instruction metadata finalization | `0x10201cb0` | `0x10254d30` |

Their command builders independently emit the same store selectors:

| store | Direct3D | OpenGL |
|---|---:|---:|
| fixed VS internal constants `32030400` | `0x1015ada1` | `0x10205431`, `0x10205511` |
| VS program `31030000` | `0x1015af74` | `0x10205694` |
| PS ALU `20030000` | `0x1015b857` | `0x10205f77` |
| PS TAU `21030000` | `0x1015b8d0` | `0x10205ff0` |
| PS API banks `22030000/80` | `0x1015c181`, `0x1015c1f1` | `0x102068a1`, `0x10206911` |
| PS internal banks `22030040/c0` | `0x1015c384`, `0x1015c3f1` | `0x10206aa4`, `0x10206b11` |

The symbol-rich Linux 5.75 compiler supplies routine names, complete compiler
objects, executable offline differentials, and the mandatory target-
configuration-2 finalizer. The Windows copies independently rule out Linux-
specific packet conventions. Old Mesa UniChrome supplies API lifecycle and
fallback policy only; it is not Chrome9 ISA evidence.

Confidence labels used below are:

* **live**: exact word executed on VX900 silicon in the current bring-up;
* **compiler-exact**: reproduced by independent proprietary compiler paths,
  but not yet executed in this new driver;
* **field-exact**: bit layout is cross-compiler exact, while arbitrary
  combinations still need numerical execution tests;
* **forbidden**: encoding may be statically visible, but runtime semantics or
  safe scheduling is not closed.

## Program stores and termination

All shader stores use counted H3 packets:

```text
FE03043C payload_dword_count selector CC000000
payload...
```

| selector | element width | hard accepted limit |
|---:|---:|---:|
| `20030000` | four dwords per PS ALU | 96 records |
| `21030000` | one dword per PS TAU | 32 records |
| `2203xxxx` | one dword per PS float constant | c0..c31 API mapping below |
| `31030000` | four dwords per VS instruction | policy 256 records initially |
| `3203xxxx` | one dword per VS float constant | c0..c255 |
| `33030000` | packed integer dword | i0..i15 |

There is no executable END instruction in either store. PS termination is the
ALU/TAU counts in A92 plus the counted packet lengths and transition maps.
VS termination is A60's active-last field plus the separately counted upload
length. D3D END tokens are consumed by the proprietary frontend. `CC000000`
and `DD000000` are command-packet padding/terminators, not shader opcodes.

Reject zero-output programs and require one complete oC0 write. Never append
an invented END record. Every payload byte after the declared count must be
zeroed or known command padding, and all unused in-memory record slots must be
zero so stale code cannot be confused with the active program by validation.

## Pixel execution controls are part of the program

The complete PS control transaction is type-01 A90--A9b followed by ALU and,
when present, TAU stores:

```text
A90 = compiled[2c] OR compiled[6c]  # bit18 selects ALU first
A91 = compiled[70]
A92 = alu_count | (tau_count << 16)
A93,A94 = TAU-to-ALU transition bitmap
A95..A9a = ALU-to-TAU transition bitmap
A9b = kernel-owned runtime state
```

Each transition word carries 24 positions. A93/A94 cover TAU 0..31;
A95..A9a cover ALU 0..95. A set bit switches stores after that record. Counts,
start phase, transition maps, TAU dependency bits, and ALU dependency fields
must be generated and verified as one object. Uploading pre-finalizer semantic
instructions is invalid.

The first four safe schedule shapes are exact:

```text
# ALU only, N records
A90=90058000 A91=91000000 A92=920000NN

# one TAU, then N ALUs
A90=90018000 A91=91000001 A92=920100NN A93=93000001

# two independent TAUs, then one ALU
A90=90000000 A91=91000001 A92=92020001 A93=93000002
TAU a01001b0 311001b1

# TAU, ALU, dependent TAU, ALU
A90=90000000 A91=91000001 A92=92020002 A93=93000003 A95=95000001
TAU 201001b0 b11001b1
```

Unused A94..A9a words are their subaddress with zero payload. A9b is never
copied from userspace. Initially admit only the first three shapes; dependent
sampling remains a separate live gate.

## PS ALU encoding exposed to the compiler

Each PS ALU record is four dwords. These fields are exact across Linux and
Windows compiler differentials:

| field | bits |
|---|---|
| source B modifier | dword 0 `[3:0]` |
| source B XYZW swizzle | dword 0 `[15:13],[12:10],[9:7],[6:4]` |
| source B id/class | dword 0 `[21:16]`, `[24:22]` |
| source C modifier | dword 0 `[31:28]` |
| source C WZYX swizzle | dword 1 `[2:0],[5:3],[8:6],[11:9]` |
| source C id/class | dword 1 `[17:12]`, `[20:18]` |
| source A modifier/W | dword 1 `[27:24]`, `[30:28]` |
| source A Z/Y/X | dword 1 bit 31 + dword 2 `[1:0]`, then `[4:2]`, `[7:5]` |
| source A id/class | dword 2 `[13:8]`, `[16:14]` |
| destination XYZW mask | dword 2 `[23:20]` |
| destination id/class | dword 3 `[9:6]`, `[11:10]` |
| saturate / scale / subop | dword 3 bit 12, `[15:13]`, `[19:16]` |

Swizzle selectors 0..3 are X/Y/Z/W, 4 is literal one, and 5 is literal zero.
Reject 6/7. Initial source classes are temporary 0, interpolated input 1,
constant 2, and texture/misc 3. Destination class 0 is temporary; class 2 id
zero is oC0. Expose only r0..r11 and oC0 initially.

The one-record operations recovered from both compiler families are:

| NIR-relevant operation | subop | initial status |
|---|---:|---|
| MOV | `d` | compiler-exact; v0/c0/texture-result forms available |
| ADD, SUB, MUL, MAD | `0` | field-exact; selected MUL forms corpus-exact |
| DP3 / DP4 | `2` / `3` | compiler-exact; numerical live gate required |
| EXP / LOG / RCP / RSQ | `4` / `5` / `6` / `7` | compiler-exact; numerical live gate required |
| ABS / CMP | `9` | compiler-exact; numerical live gate required |
| MAX / MIN / FRC | `b` / `c` / `e` | compiler-exact; numerical live gate required |

SUB is ADD with a negated source; FFMA maps to MAD. Initially allow source
NONE/NEG and destination NONE/SAT only. Linux and Windows disagree on several
other source modifiers, and the proprietary destination converter aliases
every odd value to SAT. Result scale permits only native identity/x2/x4/x8 and
/2,/4,/8 codes; reject reserved values rather than inheriting silent aliases.

LRP, SGE, POW, CRS, NRM, SINCOS, and DP2ADD expand into multiple records.
Their exact compiler sequences are corpus material, but they need checked
pre-expansion accounting and numerical gates before entering the runtime
allowlist.

## Texture instructions

The one-dword TAU layout is field-exact:

| bits | meaning |
|---:|---|
| `31:29` | class/dependency bits, finalized by scheduling |
| `28` | finalizer dependency bit |
| `27:24` and `3:0` | duplicated sampler id |
| `23:20` | operation: TEXLD=1, TEXLDB=3, TEXKILL=4, TEXLDP=7 |
| `19:16` | allocated temporary destination |
| `17:12` | allocated coordinate/source; overlaps destination bits 17:16 |
| `11:4` | four two-bit XYZW selectors |

Raw semantic words for `texld/texldb/texldp r0,t0,s0` are
`201001b0/203001b0/207001b0`. The ordinary finalized one-sample word is
`301001b0`. Independent two-sample words are `a01001b0 311001b1`.

TEXLD is the only first-stage operation. TEXLDB and TEXLDP have exact opcode
encodings but require LOD/projective live tests. TEXKILL changes scheduling
and fragment side effects and remains forbidden. Validate logical sampler ids
before encoding; s16 can collide numerically with finalized s0 because its
unchecked high carry enters bit 28. Native fields cover s0..s15, proprietary
OpenGL exposes eight, and initial Mesa should admit only the live-linked s0
and s1.

## Fragment constants and uniform updates

PS 2.0 constants are raw IEEE-754 dwords. Never apply the legacy PS 1.x C1
clamp. The API-visible mapping is discontiguous:

```text
c0..c15   native ids  0..15, selector 22030000 + first_dword
c16..c31  native ids 32..47, selector 22030080 + (first_dword - 64)
```

For a dirty run beginning at API constant `cN` and spanning `K` vectors:

```text
N < 16:  selector = 22030000 | (4*N)
N >=16:  selector = 22030080 | (4*(N-16))
count = 4*K dwords
```

Split any run crossing c15/c16. The internal stores at selectors `22030040`
and `220300c0` contain seven vec4 values each and are not user uniforms.
Never coalesce across them. Reject c32+: c32..c47 collide with the second
internal/gap bank and c48..c63 alias c0..c15.

Both Windows uploaders emit complete 64-dword API banks and 28-dword internal
banks. Sparse dirty runs are independently implemented by Linux. A new driver
may begin by uploading the complete used API bank on every program/constant
change; sparse runs are an optimization. For the initial glamor forms, upload
only c0 or c0/c1 as one eight-dword packet under `22030000`.

The constant packet must precede the draw which consumes it. A program change
must not inherit a driver-owned RGBX alpha constant as a user uniform: the
`vec4(tex0.rgb,1)` key owns c0.x=`3f800000`, and the following user-c0 program
must overwrite all four c0 lanes.

## Fragment inputs and interpolation linkage

The six-bit PS input id is an encoding width, not an advertised 64-varying
limit. Proven routes are:

* VS oD0 -> raster primary color -> PS interpolated input v0;
* VS oT0.xy -> stage-0 coordinate -> TAU t0/s0;
* VS oT1.xy -> stage-1 coordinate -> TAU t1/s1 in the exact two-texture path.

The post-transform path can keep the live Xorg VS and its D0/D3/D4 immediate
declarations. This is the lowest-risk route for the glamor fragment expansion;
it does not require a native NIR VS backend.

For a generic native VS, output linkage is explicit. With point sprites off:

```text
A25.low24 = 00f00000
            | (oD0 ? 00040000 : 0) | (oD1 ? 00020000 : 0)
            | (oFog ? 00010000 : 0)
            | tc0_format<<12 | tc1_format<<8 | tc2_format<<4 | tc3_format
A26.low16 = tc4_format<<12 | tc5_format<<8 | tc6_format<<4 | tc7_format
dimension 1/2/3/4 -> format nibble 8/c/e/f
```

Concrete values are `25f40000` for oD0, `25f4c000` for oD0+T0.xy, and
`25f4cc00` for oD0+T0/T1.xy. A28 is not rebuilt from A25: only A28 bits 18:17
follow the metadata; preserve every other initialized bit.

Secondary-color v1, arbitrary generic inputs, flat/noperspective qualifiers,
centroid/sample interpolation, point sprites, and two-sided colors are not
live-linked. Reject them. TAU coordinates follow the fixed texture-stage
perspective state; ordinary glamor S/T uses the already proven path.

## Vertex instructions, constants, and activation

A VS record is four dwords. Source word bit 0 is negate, bits
`13:2` hold W/Z/Y/X three-bit swizzles, and bits `22:14` hold a unified
nine-bit source index. Unified bases are input 32, temporary 64, integer 96,
Boolean 112, and float constant 256. Destination/control bits are mask
`3:0`, unified destination `9:4`, saturation bit 14, operation `21:17`, and
relative selector `27:24`.

Hard logical bounds are v0..v15, r0..r11, c0..c255, i0..i15, b0..b15,
oPos, oD0/oD1, oFog/oPts, and oT0..oT7. Initial policy is narrower: float
ALU only, r0..r11, v0..v15, c0..c255, oPos plus at most oD0 and T0/T1.xy,
NONE/NEG sources, NONE/SAT destination, and no relative addressing or flow.

User VS float dirty runs use the subtype-32 store rooted at selector
`32030400`; each vec4 advances the selector by four dwords. The proprietary
full path emits 1024 dwords for 256 vec4 constants. Keep the fixed
finalizer-owned twelve-dword upload at `32030400` tied to the selected native
template until the apparent address overlap and complete generic constant
layout have executed through the new path; do not splice arbitrary user runs
into the current fixed Xorg finalizer image merely because the selector is
decoded.

Integer i0..i15 are 16 packed dwords at `33030000`; each dword packs the low
bytes of XYZW. Booleans b0..b15 are bits 0..15 of saved A61, not an H3 store,
and must be canonicalized to zero/one. Initial native VS excludes both.

The activation transaction is:

```text
<subtype-32 constant stores>
type-04 A61, A62, A60 in that order
H3 selector 31030000, upload_count*4 dwords
```

A60=`60800000 | (active_count-1)` for the straight-line templates. A62 low
nine bits are compiler-derived resource metadata, not merely output count.
Upload count is independent from active count because the generic compiler
adds two inactive sentinels. Preserve initialized upper bits of A61/A62 and
derive only their documented fields. Policy-limit both active and uploaded
records to 256 until a larger silicon sweep is complete.

## Flow and branch boundary

PS 2.0 is straight-line; no safe PS branch encoding is recovered. Reject
loops, branches, calls, phi nodes, discard, and indirect constants.

VS structured-flow fields are statically decoded: CALL/CALLNZ use dword 1
bits 18:10 for `target-call`; IF uses dword 2 for `end-if` or
`else+1-if`; ELSE uses dword 1 for `end-else`; LOOP and REP use dword 2 for
`end-start-1`. Every displacement is nine bits. Vendor fixup arrays hold only
20 records per construct and lack complete bounds checks. Branch stack depth,
latency, and silicon execution remain unknown, so all VS flow is forbidden.
NIR should unroll small bounded loops or fall back to software `draw`.

## Glamor coverage already in the offline corpus

The matching `libglamoregl.so` embeds a base family in which source and mask
are independently a uniform color or 2-D texture, then combined as full RGBA,
mask alpha, or source alpha. The fifteen exact forms cover:

| glamor expression | native key status |
|---|---|
| solid `c0` | exact |
| copy `tex0` | live-equivalent exact |
| `c0*c1`, `tex0*c0`, `tex0*tex1` | exact full-RGBA composite |
| `c0*c1.aaaa`, `c0.aaaa*c1` | exact component-alpha uniform forms |
| `tex0*c0.aaaa`, `tex0.aaaa*c0` | exact one-texture component-alpha forms |
| `vec4(tex0.rgb,1)` | exact two-ALU candidate; not live |
| `tex0*tex1.aaaa` and `tex0.aaaa*tex1` | missing; generic compiler optimization loses required temporary swizzle |

The corpus also contains v0, v0*c0, tex0*v0, and v0/v1 alpha products for
fixed-function and future varying tests. Every accepted object includes exact
A90--A9b, instruction stores, constant/varying/sampler masks, and zeroed
slack. This is enough to replace the runtime's expression classifier with the
offline key classifier before introducing arbitrary ALU programs.

Repeat-coordinate repair, gradient shaders, glyph bit extraction, conditional
discard, dual-source output, and multi-sample YUV conversion remain outside
this subset. Hardware sampler repeat/clamp/mirror should avoid glamor's
`RepeatFix` shader for ordinary pixmaps; it must not be approximated when the
large-pixmap repair path is genuinely selected.

## Smallest safe runtime expansion

### Gate 1: execute closed compiler keys

Keep software VS and existing vertex packets. Replace the five-way NIR
classifier with the offline canonical-expression classifier, but expose only
one new key at a time. For each key, Mesa supplies its logical key and used
constants/resources; the kernel selects or regenerates the exact canonical
object and checks resource masks against bound texture state. This avoids a
raw native-program ABI for the first live gates.

Recommended order:

1. v0*c0, proving a user constant and ALU-only MUL;
2. tex0*c0, proving a user constant with the live TAU schedule;
3. c0*c1, proving an eight-dword uniform upload;
4. the four one-alpha replicated products, using deliberately different RGB
   and alpha values;
5. RGBX reconstruction with stored non-one alpha and driver-owned c0.x;
6. two-texture full product under the canonical compiled object;
7. only then add a separately recovered two-texture alpha-swizzle object.

Run every key as `old -> new -> old` without context reset, and include zero,
half, one, negative, and above-one constants where the destination format and
clamp policy make the result distinguishable.

### Gate 2: bounded straight-line native object

After the closed keys pass, admit a canonical Mesa-built object containing:

* at most 96 PS ALUs and 32 TAUs, with a lower initial policy limit;
* MOV/ADD/MUL/MAD only, full XYZW oC0 store, r0..r11, c0..c31;
* v0 and linked TAU t0/t1 only;
* TEXLD only, s0/s1 only;
* one of the first three exact schedule shapes;
* explicit used-resource masks and complete zeroed object storage.

The kernel verifier must decode every admitted field, reject all reserved
bits/classes/ids/opcodes, recompute counts and transition maps, own A9b, and
cross-check constants, varyings, samplers, image BOs, and cache invalidation.
A hash or byte comparison against Mesa output is useful for diagnostics but
is not a security boundary.

### Gate 3: widen operations numerically

Add DP3/DP4, MIN/MAX, RCP/RSQ, FRC, CMP, and partial output masks one at a
time. Each operation needs a compiler-exact record, numerical CPU reference,
NaN/Inf/zero/sign cases appropriate to the operation, and transition closure.
Only after these pass should multi-record lowering, dependent texture reads,
TEXLDP/TEXLDB, or native VS be enabled.

## State-transition closure and failure policy

Each draw transaction must make the following state self-contained:

1. render/depth/blend state and target format;
2. texture images, mip tables, samplers, and texture-cache pulse;
3. VS declarations/output linkage, constants, A61/A62/A60, and VS store;
4. PS API constants, A90--A9b, ALU store, and TAU store;
5. primitive/vertex packet and retirement cookie.

An ALU-only program following a textured program must write TAU count zero,
all transition words zero, and a complete ALU-first A90. A one-texture program
following two textures must rewrite A92/A93, both stores, sampler/image state,
and linkage; no old TAU or transition bit may remain reachable. Constants
used by the new program must be overwritten before execution. Shader changes
do not replace the render-to-texture producer drain or texture-cache pulse.

On verifier failure, fall back before publication. On any timeout after ring
publication, preserve the existing engine quarantine/ownership rules; never
retry a nearby shader or free reachable BOs. A rejected native VS falls back
to Gallium `draw`. A rejected fragment program must use an explicit software
or higher-level fallback rather than silently dropping the draw.

## Evidence boundary

| item | status |
|---|---|
| store selectors, packet counts, PS 96/32 limits | field-exact in Linux and both Windows UMDs |
| PS constants c0..c31 and bank split | field-exact in Linux, D3D, and OpenGL builders |
| MOV/MUL records in fifteen keys | compiler-exact; live only where they match existing fixed paths |
| A90--A9a scheduling for three basic shapes | compiler-exact; fixed paths live |
| v0 and TAU t0/t1 linkage | v0/t0 live; t1 live in fixed two-texture path |
| VS output metadata and eight templates | compiler-exact; fixed Xorg templates live, generic templates not |
| PS general arithmetic numerical behavior | unmeasured beyond current MOV/MUL paths |
| PS branches | no supported encoding; forbidden |
| VS branch fields | statically decoded, silicon behavior unmeasured; forbidden |
| arbitrary interpolation qualifiers and generic varyings | unresolved; forbidden |

