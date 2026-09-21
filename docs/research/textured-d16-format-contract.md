# Chrome9 one-texture plus D16 and X/A format contract

Status: this note composes state groups which are independently live on VX900:
single-texture REPLACE/MODULATE, textured blending and alpha test, and linear
D16 LESS/write.  Their simultaneous one-texture-plus-D16 packet has not yet
run on silicon.  The exact word placement below is a bounded successor oracle,
not a capability claim.

Static addresses below use the PE image base for Windows and ELF virtual
addresses for Linux.  The analyzed objects are `VTGUModeDX32A.dll` SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
`VTGOGL32A.dll` SHA-256
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`,
and `via_chrome9_dri.so` SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.

## Minimal combined packet

The one-texture builder has two fragment-program lengths and one optional
four-dword destination-state expansion:

| program | legacy destination | closed blend destination |
|---|---:|---:|
| REPLACE | 224 dwords | 228 dwords |
| MODULATE | 228 dwords | 232 dwords |

D16 replaces the common group's final pad with A10 and appends A11, A12, A13,
and the already-live wide-Z A20.  The payload grows from eight to twelve
dwords, so the aligned group and whole body grow by exactly four.  Therefore:

| program | BGRX/COPY minimal D16 | state-complete blend/alpha/D16 |
|---|---:|---:|
| REPLACE | 228 dwords | 232 dwords |
| MODULATE | 232 dwords | 236 dwords |

The recommended successor is the state-complete right column even when blend
and alpha test are disabled.  It carries destination format, independent
blend closure, color mask, and the exact same shader object as the existing
textured successor.

### Destination and D16 placement

The closed destination H2 packet has 15 meaningful payload words and one
alignment slot.  Put optional fixed alpha-test A33 in that slot:

```text
body[0..3]    H2 type-01 destination header
body[4]       A50 destination base
body[5]       A51 destination layout/location
body[6]       A52 destination format/pitch
body[7..14]   A38..A3F blend constants, always closed
body[15..18]  A34..A37 RGB/alpha equations
body[19]      A33 when alpha test is enabled; cc000000 otherwise
```

This placement is not cosmetic.  The current textured blend builder uses the
common group's last payload word for A33.  D16 needs that word for A10.  If
A33 is left there, the common payload becomes 13 dwords, rounds up to 16, and
adds another four dwords to every later index.  A33 is subaddressed fixed
state and has no dependency on packet adjacency; programming it at body 19
before A00 enables it at body 47 is the compact, transition-safe order.

The texture image and sampler remain unchanged at `20..31` and `32..39`.
The common group is:

```text
body[40..43]  H2 type-01 common header
body[44]      860000ff                         # A86
body[45]      870000ff                         # A87
body[46]      A53 color mask/RT conversion
body[47]      A00 blend and alpha-test enables
body[48]      A01 raster/depth enables
body[49]      88001ed0                         # A88
body[50]      89000800                         # A89
body[51]      10000000 | (depth_sf_va >> 8)    # A10
body[52]      11000001 | ((depth_pitch/32)<<13)# A11
body[53]      12380000                         # A12 D16/SF/linear
body[54]      13000000 | (depth_func << 16)    # A13
body[55]      20007fff                         # A20 wide Z clip
```

Require depth pitch to be canonical `ALIGN(width*2,32)`, nonzero, and exactly
representable in A11; validate the full guarded D16 extent independently of
the color and texture extents.  The first combined gate remains LESS/write,
so A13 is `13010000`.

The precise A01 word depends on the fragment path:

```text
REPLACE + D16:   01c0154c
MODULATE + D16:  01ce154c
```

Do not replace MODULATE's A01 with the untextured depth word.  Its bits
19/18/17 preserve fog, specular, and diffuse perspective-correction state
from the proprietary GL image; D16 adds only bits 3/2.  Type-02/FE A01 remains
`01002521` for texture-coordinate perspective and every D0.w remains the
finite reciprocal W.  D16 changes no H6 FVF, VS, PS, TAU, ALU, or vertex
stride word.

### Complete compact-builder index maps

The implementation in kernel commit `2e78ad0` retains the compact non-blend
destination image.  Alignment pads are part of the native body and must be
counted; the common prefix is:

| dwords | block |
|---:|---|
| `0..14` | destination header/image, including A52 at `6` |
| `15` | alignment pad |
| `16..25` | texture image header/state; image A30 at `25` |
| `26..27` | alignment pads |
| `28..35` | sampler header/state; sampler A30 at `32` |
| `36..51` | common render and D16 state |
| `52..60` | type-02/FE one-texture configuration |
| `61..63` | alignment pads |
| `64..93` | H6 FVF, raster, viewport linkage |
| `94..95` | alignment pads |
| `96..103` | PS constants |
| `104..115` | VS float constants |
| `116..123` | VS integer constants |
| `124..131` | color/scissor windows |
| `132..155` | five-instruction VS |
| `156..160` | VS activation |
| `161..163` | alignment pads |
| `164..180` | PS controls |
| `181..183` | alignment pads |
| `184..188` | one TAU |
| `189..191` | alignment pads |

The tails differ only in program size:

```text
REPLACE:  body[192..199] ALU packet; body[200..227] H6 header + 3*8 vertices
MODULATE: body[192..203] ALU packet; body[204..231] H6 header + 3*8 vertices
```

High-value construction assertions are:

```text
[6]       5298pppp                         # BGRX target A52; 5299pppp for BGRA
[20]      texture_base >> 8
[21]      18001101                         # SF image location
[22]      texture dimensions in image A20
[23]      21000001
[24]      image A22 logical-size word
[25]      30980000 (BGRX) or 30990000 (BGRA)
[32]      30000000                         # sampler A30, explicit level zero
[43]      A00
[44]      01c0154c or 01ce154c
[47..51]  A10, A11, A12, A13, A20
[80..82]  20000000 23004f02 24000003
[184]     FE020440                         # TAU header
[192]     FE020440                         # ALU header
[200/204] FE020440                         # REPLACE/MODULATE vertex header
```

Type-01 A20 at body 51 is the high-byte near/far Z clip word.  Type-02 image
A20 at body 22 contains texture dimensions, and type-04 A20 at body 80 is the
primitive-reuse word.  The validator and diagnostic decoder must keep the H2
selector/type with every repeated subaddress.

Engine initialization and the live D16 path establish A1E/A1F as
`1e000000/1f000000`; the compact packet re-emits only A20.  This is safe only
while every kernel-generated path preserves the two low words.  If a future
viewport/depth-range path can change them, the combined builder must either
reject that transition or emit the full A1E/A1F/A20 tuple.  Adding A1E and
A1F makes the common payload 14 words and shifts every block after it by four
dwords; do not insert them without updating body length and all assertions.

## Three-object lifetime and transition hazards

The initial one-texture+D16 job has three distinct native guarded BOs in this
mapping order:

```text
target  = sf_start + PAGE_SIZE
texture = sf_start + target_object_size + PAGE_SIZE
depth   = sf_start + target_object_size + texture_object_size + PAGE_SIZE
```

Use checked additions, reject every pairwise alias, reserve all three in one
`drm_exec` transaction, concatenate PTEs in the same order, and publish only
after every mapping and fence is installed.  Target and depth take WRITE
fences; texture takes a READ fence.  Timeout quarantine retains all three BO
references, CPU maps, SG tables, PTEs, SF range, DMA ownership, and the ring
segment as one indivisible job.

Before publication, flush CPU aliases and hand all three mappings to the
device.  Never perform a pre-submit sync-for-CPU: on the measured 32-bit
DMA/SWIOTLB path that copied stale bounce contents over initialized color and
depth.  After proven retirement, return DMA ownership, then inspect all six
guards and useful ranges.  A texture produced by an earlier render must first
retire its write fence and then pass through the established type-02 A00
texture-cache-clear transition; do not allow target==texture feedback in the
first ABI.

The packet itself must always close both sides of a transition:

- texture+D16 -> texture without depth clears A01 bits 3/2 before the next
  immediate draw; stale A10--A13 addresses are then inert;
- alpha+D16 -> alpha disabled writes A00 with bit 20 clear before drawing;
- MODULATE -> REPLACE restores A01 from `01ce...` to `01c0...` and uploads
  the complete REPLACE controls/TAU/ALU object;
- every texture draw writes type-02/FE A00 with the one-stage enable and
  one-shot cache clear, sampler A30 with explicit level zero, and the complete
  A90 control image.

There is no evidence for an additional ordering packet between texture and
D16 state.  Both proprietary stacks build them as ordinary dirty HPara state
consumed by a later primitive.  The safety boundary is complete state before
the immediate header, correct BO ownership, and bounded retirement—not a
guessed intermediate flush.

## Native X-versus-alpha sampling semantics

A30 `[23:16]` is a format decoder, not a swizzle register:

| A30 | packed word | host bytes | logical sampled result |
|---:|---|---|---|
| `98` X8R8G8B8 | `xxRRGGBB` | B,G,R,X | R,G,B,1 |
| `99` A8R8G8B8 | `AARRGGBB` | B,G,R,A | R,G,B,A |
| `b8` X8B8G8R8 | `xxBBGGRR` | R,G,B,X | R,G,B,1 |
| `b9` A8B8G8R8 | `AABBGGRR` | R,G,B,A | R,G,B,A |

The VX900 manual explicitly says every format without an alpha channel fills
sampled alpha with 1.0.  The executable Windows D3D lookup independently maps
private formats `2030/2031/2038/2039` to A30
`99/98/b9/b8` at `0x1020a660..0x1020a7c4`.  Its RT-only lookup at
`0x1020a4c0..0x1020a57d` maps those same private formats to the same A52
bytes.  These are paired storage interpretations; there is no channel-order
fixup in A31/A32.

The proprietary Linux ICD supplies an additional semantic cross-check in its
software texture fallback.  `__glExtractTexelXRGB8:0x329620` ignores the
stored high byte and writes output alpha `ff` at `0x329667`; its border path
also writes `ff` at `0x3296cc`.  `__glExtractTexelARGB8:0x3293b0` instead
copies the stored high byte to output alpha at `0x3293f7..0x3293fc`.  The
`_B` and 3D variants repeat the same distinction.  This is not direct silicon
proof, but it independently fixes the API semantics intended by the same
driver family.

Normal X-format sampling therefore needs no fragment-program alpha repair.
The existing identity sample/MOV or RGBA MODULATE program consumes native
`R,G,B,1`.  The separately recovered two-ALU `vec4(tex.rgb,1)` shader key is
for an explicit programmable RGBX construction or view swizzle; using it for
ordinary A30=`98/b8` would waste a constant and obscure whether the format
decoder works.

Arbitrary Gallium sampler-view swizzles remain shader operations.  Accept
native format identity only: BGRX/RGBX views obtain constant-one alpha from
the decoder, BGRA/RGBA views obtain stored alpha, and every nonidentity
R/G/B/A/0/1 view swizzle must match a separately validated shader key.

### Exact X/A discriminator

Use a format-99 BGRA destination with blending disabled and the pure REPLACE
program.  A 2x2 source contains the same red RGB with four poison high bytes:

```text
00ff0000  40ff0000
80ff0000  ffff0000
```

With A30=`98`, all four destination dwords must be `ffff0000`.  With
A30=`99`, they must remain exactly the four source dwords.  This varies only
the source-format selector and observes alpha directly without blend
rounding, vertex-color normalization, or a shader swizzle.  Repeat the same
idea for A30=`b8/b9` using logical-red source words
`000000ff/400000ff/800000ff/ff0000ff`; expected format-99 destination words
are again all `ffff0000` for `b8` and the four alpha-varying red dwords for
`b9`.

## Render-target reinterpretation boundary

A52 uses the same four format bytes, and the manual gives the same
constant-one rule when the destination has no alpha channel.  A52=`98/b8`
therefore discards alpha writes and supplies 1.0 to destination-alpha blend
factors; A52=`99/b9` stores and reads real alpha.  The live format-99 COPY,
alpha-only mask, and independent-alpha source-over results prove the latter.

Changing A52 or A30 never converts memory.  It only changes how identical
bits are decoded.  The proprietary Windows builders both derive their format
from the resource's single private format at `surface+0x34`; they do not take
an independent typeless view format.  No recovered Linux or Windows path
authorizes arbitrary X/A or R/B reinterpretation of one resource.

Keep the first Gallium implementation strict:

```text
surface/view format == resource storage format
level == 0, first_layer == last_layer == 0
```

In particular, do not view an X resource as A: its high byte is semantically
undefined and A30/A52=`99/b9` would expose it as real alpha.  Viewing A as X
would deliberately discard stored alpha and force one, but remains gated
until an API compatibility rule and a guarded transition test justify it.
Linear/sRGB twins are a separate controlled exception: storage A52 remains
`98/99/b8/b9`, while dynamic A53 and the corresponding sampler conversion
state select color-space conversion.  They are not evidence for general
same-bpp reinterpretation.

For a diagnostic only, switching one retired BGRA BO between A52=`99` and
A30=`99` can prove render-to-texture ownership.  It still requires producer
retirement, the texture-cache clear, and a different destination BO; never
bind that BO simultaneously as source and target in the bounded successor.

## Default-alpha visual and textured-D16 successor

The source and destination alpha choices are independent.  A normal GL RGB
texture stored as BGRX needs image A30=`30980000`; the decoder supplies alpha
one and the existing REPLACE or RGBA MODULATE program needs no repair
instruction.  An RGBA texture needs `30990000` and contributes its stored
alpha.  A default GLX alpha-8 drawable is a different decision: its render
target needs A52 format `99` so fragment alpha is retained.  Selecting A52
`98` for that drawable silently forces/discards target alpha even if the
sampler and fragment program are otherwise correct.

The Windows D3D UMD makes the separation executable.  Its RT-only lookup at
`VTGUModeDX32A.dll:0x1020a4c0` reads the surface format at `+0x34` and maps
private `2030/2031` to `99/98`; its broader texture lookup at `0x1020a660`
maps the same values to the same bytes.  Neither lookup receives a shader or
view-swizzle argument.  The Linux ICD supplies an independent semantic
check: `__glExtractTexelARGB8:0x3293f7..0x3293fc` copies the stored high byte,
whereas `__glExtractTexelXRGB8:0x329667` writes output alpha `ff`; the XRGB
border path repeats that constant at `0x3296cc`.

The first textured-D16 ABI deliberately fixes destination format zero/BGRX.
Extending it for an alpha-8 visual changes exactly one native word:

```text
body[6] BGRX  = 52980000 | (destination_pitch >> 5)
body[6] BGRA  = 52990000 | (destination_pitch >> 5)
body[43] A00  = 00000011                  # no blend or alpha test
body[44] A01  = 01c0154c / 01ce154c       # REPLACE / MODULATE plus D16
body[50] A13  = 13010000 / 13030000       # LESS / LEQUAL
```

No FVF, TAU, ALU, texture format, packet count, or BO mapping changes.  A53
stays `53010c0f`, so all four channels are written.  In particular, do not
route this through framebuffer blending merely because the target has alpha.

A binary-compatible bounded UAPI extension can consume the first reserved
dword of `drm_chrome9_draw_textured_triangle_d16` as
`destination_format`, using the existing enum (`0` BGRX, `1` BGRA) and a new
capability bit.  Old kernels continue to accept format zero and reject the
new nonzero word.  New kernels must reject values above one, pairwise BO
aliasing, blending, alpha test, and partial color masks; the three-object
reservation, READ/WRITE fences, guard verification, and timeout quarantine
remain byte-for-byte unchanged.  Mesa should require the new capability only
for a BGRA target and must not treat the existing overloaded
`blend_successor` Boolean as proof that actual blending is enabled.  The
allow condition is full RGBA mask, blend disabled, alpha test disabled, one
texture, guarded BGRA target, guarded Z16, and LESS or LEQUAL.

The smallest silicon discriminator uses a 32-square format-99 target, a 2x2
format-99 texture containing opaque white, and D16 prefilled to `8000`:

1. MODULATE a vertex color `80402010` at incoming depth `4000`; require the
   covered target dword `80402010` and D16 `4000`.
2. Draw a different color at depth `c000`; require both target and D16 to
   remain unchanged.
3. Repeat step 1 with target A52=`98`; require RGB `00402010` and use the
   existing XRGB readback convention rather than asserting its stored high
   byte.

For the sampler discriminator, use a format-99 destination and identical
source dwords `00ff0000/40ff0000/80ff0000/ffff0000`.  A30=`99` must preserve
the four alpha values; A30=`98` must produce `ffff0000` four times.  Sequence
`99 -> 98 -> 99` without a context reset and require the first result again;
this proves format closure rather than a fortunate power-on default.  All
tests use separate guarded source/target/depth BOs and check all six guards
after retirement.
