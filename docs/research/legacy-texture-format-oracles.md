# Chrome9 legacy texture and render-format oracles

This note closes the static encoding and test plan for the small formats most
likely to occur in ioquake3-era assets and glmark2 uploads: A8, L8, L8A8,
RGB565, A1R5G5B5, and A4R4G4B4.  All encodings are exact manual plus
proprietary-driver recoveries.  They remain capability-gated until the
bounded tests below retire on VX900.

The sampler and render-target format tables are related but not identical.
A8 and L8 are native sampler formats but are rejected by the render-target
lookup.  L8A8 and the three 16-bit RGB(A) formats are present in both tables.
Do not advertise render support merely because a sampler A30 byte exists.

## Executable provenance

The relevant Windows binaries are:

```text
VTGUModeDX32A.dll
  sha256 8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll
  sha256 a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
```

The D3D public-format converter at `VTGUModeDX32A:0x100c7590` supplies these
private identities:

| public D3D9 format | private format |
|---|---:|
| A8 | `0806` |
| L8 | `0805` |
| A8L8 | `1029` |
| R5G6B5 | `1018` |
| A1R5G5B5 | `1019` |
| A4R4G4B4 | `101b` |

The D3D sampler lookup at `0x1020a660` maps those private values to
`1b,13,15,89,8a,8b`.  The OpenGL ICD has the equivalent lookup at
`0x1025d590`; its return blocks include A8 `0x1025d5de`, L8 `0x1025d5e5`,
L8A8 `0x1025d64c`, RGB565 `0x1025d661`, A1R5G5B5 `0x1025d65a`, and
A4R4G4B4 `0x1025d653`.  The two lookup tables are the same after relocation.

The stricter render-target lookup is D3D `0x1020a4c0..0x1020a57d` and OpenGL
`0x1025d3f0..0x1025d4ad`.  It maps private `1029,1018,1019,101b` to
`15,89,8a,8b`; it returns `ffffffff` for private A8 `0806` and L8 `0805`.
The Linux proprietary RT lookup independently exposes the same supported
subset, as recorded in
[render-target-format-oracles.md](render-target-format-oracles.md).

The VX900 manual, printed pp. 124--126, names the same texture fields and
states the missing-channel rules.  Its RT0 A52 table, printed pp. 106--108,
names L8A8 and the same three RGB16 formats.  Old open Mesa independently
selects A8/L8/AL88/RGB565/ARGB1555/ARGB4444 in
`Mesa-7.11.2/src/mesa/drivers/dri/unichrome/via_tex.c:449..473`; its register
definitions at `via_3d_reg.h:970..1010` give the same family/member values.
That older path is useful semantic corroboration, while the two Chrome9
Windows lookups are the generation-specific proof.

## Exact sampling matrix

The image format word is type-02 image subaddress A30.  No additional channel
swizzle is hidden in the other A30 bits.

| logical format | Gallium format | A30 | bytes/texel | packed memory | sampled RGBA |
|---|---|---:|---:|---|---|
| A8 | `PIPE_FORMAT_A8_UNORM` | `301b0000` | 1 | `A[7:0]` | `0,0,0,A` |
| L8 | `PIPE_FORMAT_L8_UNORM` | `30130000` | 1 | `L[7:0]` | `L,L,L,1` |
| L8A8 / AL88 | `PIPE_FORMAT_L8A8_UNORM` | `30150000` | 2 | word `AAAA AAAA LLLL LLLL` | `L,L,L,A` |
| R5G6B5 | `PIPE_FORMAT_B5G6R5_UNORM` | `30890000` | 2 | `RRRRRGGG GGGBBBBB` | `R,G,B,1` |
| A1R5G5B5 | `PIPE_FORMAT_B5G5R5A1_UNORM` | `308a0000` | 2 | `ARRRRRGG GGGBBBBB` | `R,G,B,A` |
| A4R4G4B4 | `PIPE_FORMAT_B4G4R4A4_UNORM` | `308b0000` | 2 | `AAAARRRR GGGGBBBB` | `R,G,B,A` |

The Gallium names describe increasing little-endian byte addresses.  Thus an
L8A8 word has L in the lower-address byte and A in the next byte.  Likewise,
the `B5G6R5`, `B5G5R5A1`, and `B4G4R4A4` names are the correct Mesa views of
the manual's numeric RGB565/ARGB1555/ARGB4444 words.

Missing alpha is exactly one, not undefined.  A8 supplies zero to all three
color channels.  L8 and L8A8 perform the legacy format swizzle in the texture
decoder; shader lowering must not replicate luminance or alpha a second time.
Arbitrary sampler-view swizzles still belong in the shader.

For a canonical linear level, derive the pitch as
`ALIGN(width * bytes_per_texel, 32)`.  There is no independent texture-pitch
register.  The vendor builder retains logical A20 width when the caller pitch
equals this canonical value; a noncanonical pitch is represented by replacing
the effective width and is outside the first ABI.  Require a 256-byte-aligned
base and cover at least
`pitch * ALIGN(height, 8)` bytes between guards so the same allocation rule
continues to work when mip levels are added.

At 32x32, A8 and L8 use pitch 32 and a `0x400`-byte covered range.  Every
16-bit format uses pitch 64 and a `0x800`-byte range.  A20 remains
`20020020`, A22 remains `22008055`, and nearest/clamp sampling remains
`30000000 31100000 32000049`; only image A30, derived pitch/range, and source
contents change.

## Exact render-target subset

For a linear SF render target:

```text
A50 = 50000000 | (sf_address >> 8)
A51 = 51100000
A52 = 52000000 | (format_byte << 16) | (byte_pitch >> 5)
A53 = 53010c0f
```

The common 32x32 16-bit target has byte pitch 64, so its exact words are:

| target | A52 | stored word | alpha semantics |
|---|---:|---|---|
| L8A8 | `52150002` | A high byte, L low byte | stored A |
| RGB565 | `52890002` | R5:G6:B5 | implicit A=1 on destination reads |
| A1R5G5B5 | `528a0002` | A1:R5:G5:B5 | stored A |
| A4R4G4B4 | `528b0002` | A4:R4:G4:B4 | stored A |

A8 and L8 have no A52 mapping in either Windows RT-only lookup.  Report them
sample-only.  Do not feed their sampler bytes `1b` or `13` to A52.

A50 has 256-byte address granularity and linear A52 pitch has 32-byte
granularity.  Retain the conservative Windows-producer limit
`byte_pitch <= 16352`, validate `pitch * height` against the BO with overflow
checks, and keep two-sided guards.  The first target tests remain linear; a
format's presence says nothing about tiled layout, scanout, or KMS fourcc
support.

For L8A8, the format table proves bit placement but does not state which of
unequal fragment RGB components is reduced to L during a store.  Start with
gray fragment colors, for which R=G=B=L is unambiguous.  A later distinct-RGB
probe must identify the reduction rule before general L8A8 rendering is
advertised.

## Bounded sampler readback oracles

Use the already-live one-texture REPLACE triangle, a BGRA8888 destination
(A52=`99`), normalized coordinates, nearest filtering, and four constant
quadrants.  BGRA preserves sampled alpha and makes every expected dword
visible without introducing blending.  Change no shader, FVF, sampler, wrap,
or packet length.

| source A30 | quadrant source values | expected BGRA8888 dwords |
|---:|---|---|
| A8 `301b0000` | `00,55,aa,ff` | `00000000,55000000,aa000000,ff000000` |
| L8 `30130000` | `00,55,aa,ff` | `ff000000,ff555555,ffaaaaaa,ffffffff` |
| L8A8 `30150000` | `0000,ff00,0080,ffff` | `00000000,ff000000,00808080,ffffffff` |
| RGB565 `30890000` | `f800,07e0,001f,ffff` | `ffff0000,ff00ff00,ff0000ff,ffffffff` |
| A1R5G5B5 `308a0000` | `7c00,fc00,83e0,801f` | `00ff0000,ffff0000,ff00ff00,ff0000ff` |
| A4R4G4B4 `308b0000` | `0f00,ff00,f0f0,f00f` | `00ff0000,ffff0000,ff00ff00,ff0000ff` |

Read only safely interior samples at quadrant centers.  Fill all row/allocation
padding with format-specific poison values, validate the entire source and
destination ranges after retirement, and retain both BO mappings through any
timeout quarantine.  A channel-order failure produces another listed primary
rather than an unmapped fetch.

After each nearest decoder passes, test linear filtering separately.  L8 at a
`00/ff` boundary must yield equal RGB in `{7f,80}` and A=`ff`; A8 must yield
RGB zero and A in `{7f,80}`.  L8A8 endpoints `(L=0,A=ff)` and `(L=ff,A=0)`
must yield all four channels in `{7f,80}`.  These cases prove that filtering
occurs on decoded components, but they should not be mixed into the first raw
format gate.

## Bounded render and blend oracles

Allocate each candidate 32x32 target at pitch 64 between guards and prefill
it with a nonzero poison word.  Use COPY/REPLACE and full-channel A53 mask.
Draw only an interior rectangle, wait for retirement, and compare raw 16-bit
memory.  The exact extreme-value expectations are:

| target A52 | fragment colors | expected words |
|---:|---|---|
| RGB565 `52890002` | red, green, blue, white | `f800,07e0,001f,ffff` |
| A1R5G5B5 `528a0002` | transparent red, opaque red, opaque green, opaque blue | `7c00,fc00,83e0,801f` |
| A4R4G4B4 `528b0002` | transparent red, opaque red, opaque green, opaque blue | `0f00,ff00,f0f0,f00f` |
| L8A8 `52150002` | black/A0, white/A0, black/A1, white/A1 | `0000,00ff,ff00,ffff` |

For the L8A8 row, set R=G=B for every fragment.  Once it passes, render
opaque red, green, blue, and white with distinct alpha bytes.  The high output
bytes must follow alpha.  The low bytes determine whether L comes from R,
another component, or an RGB conversion; document that live result before
enabling arbitrary-color L8A8 targets.

Stored-alpha formats need a second source-over test.  Render transparent red
and opaque red over opaque green with RGB factors SRC_ALPHA/INV_SRC_ALPHA and
independent alpha factors ONE/INV_SRC_ALPHA.  The first sample must retain
green and the second must become red.  For A1R5G5B5 this checks both alpha
decoding and destination-alpha storage at the only two representable values;
for A4R4G4B4, follow with alpha nibble eight only after the endpoints pass.
RGB565 must be treated as destination alpha one for destination-alpha blend
factors, but that factor behavior is a separate capability gate from COPY.

If a rendered candidate is immediately sampled, order the producer fence,
retain the BO/PTEs, and emit the consumer's normal type-02/FE texture-cache
clear pulse (general A00 bit 0).  Merely changing A52 to the matching A30 code
does not make dirty render data visible.

## Implementation and fallback priority

Keep format support split by bind class.  A safe rollout order is:

1. Add native **sampler-only RGB565**.  It is common, has no alpha ambiguity,
   and changes only cpp, canonical pitch/range, and A30=`89`.
2. Add native **sampler-only L8**, then **A8** and **L8A8** after BGRA alpha
   readback passes.  This covers legacy light/intensity/alpha assets without
   adding render-target state.
3. Add sampler A1R5G5B5 and A4R4G4B4 after the endpoint alpha tests.
4. Add RGB565/A1R5G5B5/A4R4G4B4 render binds one at a time after COPY, clear,
   mask, blend, render-to-texture, and guard tests.  Add L8A8 rendering only
   after its RGB-to-L reduction is measured.  Never expose A8 or L8 render
   binds on this evidence.

Until a native sampler format passes, Mesa can safely expand uploads into the
already-live BGRA8888 texture representation:

```text
A8       -> (0, 0, 0, A)
L8       -> (L, L, L, ff)
L8A8     -> (L, L, L, A)
RGB565   -> (R8, G8, B8, ff)
A1RGB555 -> (R8, G8, B8, A1 ? ff : 00)
A4RGB444 -> (R8, G8, B8, replicate(A4))
```

Use ordinary UNORM bit replication, not a scale with truncation:
`v5_to_v8=(v<<3)|(v>>2)`, `v6_to_v8=(v<<2)|(v>>4)`, and
`v4_to_v8=(v<<4)|v`.  These formulas preserve both endpoints and match the
decoder expectations used by the raw quadrant tests.

Perform that conversion at resource upload/transfer time and retain the
32-bit expanded resource as the texture's actual backing.  Do not silently
reinterpret packed memory as BGRA.  Expansion preserves sampling and mipmap
generation semantics at the cost of memory and bandwidth; it does not emulate
an exact packed render target or external shared-image layout.  If exact
packed render/storage is requested before native RT validation, reject that
bind or choose an API-permitted 32-bit internal backing rather than promising
unsupported storage.

The kernel UAPI should continue to accept a normalized format enum and derive
A30/A52, cpp, pitch, and range itself.  Never expose raw format bytes or let
userspace pair a 1-byte cpp with a 16-bit selector.  Mesa's
`is_format_supported` must return sampler, render-target, blendable, and
scanout support independently; none of the static table rows justifies all
four usages.
