# Minimal truthful Mesa texture path

## Implemented and live-validated milestone (2026-09-21)

The first real Gallium texture path is implemented behind
`DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE` (capability bit 6, ioctl `0x05`).
It accepts one guarded, linear level-zero B8G8R8X8/B8G8R8A8 2D texture,
canonical 32-byte-aligned pitch, nearest or linear filtering, edge/repeat/
mirrored-repeat wrap, and the bounded REPLACE or MODULATE fragment programs.
The eight-dword post-TNL vertex preserves reciprocal W and TEXCOORD0.

On the VX900 target, the raw structured ioctl passed REPLACE and MODULATE with
2,080 covered pixels and exact green/white/outside samples.  The Gallium smoke
then passed a 2x2 red/green/blue/white REPLACE draw and a channel-sensitive red
MODULATE draw, with two submitted triangles per quad and no rejections.  It
verified every source pixel, row and aligned-tail padding, and every dword of
both guard pages.  The untextured color, D16, resource and PRIME regressions
also passed, and no new kernel error was logged.

The corresponding milestones are kernel driver commit `93d0332` and Mesa
driver commits `942aa0f` plus `63a6a86`.  The follow-up varying-reciprocal-W
Gallium test also passed: REPLACE selected white texel 3 rather than the yellow
affine diagnostic at x=48, while MODULATE produced the expected perspective-
interpolated red near `0x70` rather than the affine value near `0xc1`.  This
independently proves D0.w preservation plus FE/type-02 texture perspective and
type-01 diffuse perspective state.  The test also covers a REPLACE vertex
shader with COLOR0 optimized away by using the compact POS/TEX vertex-element
layout.

## Smallest hardware implementation

The measured 232-dword oracle in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md) is enough for one
linear 2D level-zero sampler.  A first structured ioctl needs the following
bounded inputs, not raw packets:

- destination and source GEM handles;
- destination width, height, and pitch plus source power-of-two logical width,
  power-of-two height, and canonical pitch;
- one of two kernel-known formats: B8G8R8X8 (`A30=98`) or B8G8R8A8
  (`A30=99`) for the source, and the already-proven B8G8R8X8 destination;
- three vertices containing bounded screen `x/y/z`, finite reciprocal clip
  `w`, packed primary color, and finite `s/t` coordinates;
- program selector `REPLACE` or `MODULATE` and bounded min/mag/mip/S/T wrap
  enums.

The kernel must look up and retain both BOs, lock their reservations in a
stable order, wait for prior writers, map every participating page through
the RTSF table with cached-shmem PTE bit 25, perform the established CPU/device
ownership transition, and keep the BOs/PTEs alive until the command fence.
After publishing any new PTEs it must perform the bounded GTI invalidation,
then pulse general-texture A00 bit 0 before fetching.  For render-target to
texture reuse, the producer fence/idle precedes that pulse.  An engine-idle
read alone does not establish CPU or translation-cache ownership.

For the translated domain the packet relocations are:

```text
destination A50 = 50000000 | (destination_sf_va >> 8)
destination A51 = 51100000                 # linear, location 01=SF
destination A52 = 52980000 | (pitch >> 5) # X8R8G8B8
texture A00     = source_sf_va >> 8
texture A18     = 18001101                 # ordinary 2D, relative, 01=SF
texture A30     = 30980000 or 30990000
```

Use the oracle's `A01=01002521`, `A08=08010101`, `A05=0`, `A0D=0d810000`
linkage and its texture-cache pulse.  The textured post-TNL vertex declaration
is exactly:

```text
FVF A00 = 00000308
D3      = 04006342
A62     = 623081ff
VS tail = 0008c14c 00000000 00000000 0008028f
A60     = 60800004
```

Do not "correct" A08 by setting its texture-A S/T input-mode bits merely
because Gallium calls the coordinates normalized.  In `08010101`, bits 13 and
12 are zero, while bits 9:8 select two-dimensional texture A and bits 3:0 say
one coordinate register.  The Xorg producer supplies already-normalized
coordinates: `via3DEmitQuad_H5:0x2e6a8-0x2e727` divides every generated S and T
by the corresponding texture width and height before vertex emission.  Thus
the manual's zero-valued "un-normalized" mode means no additional hardware
normalization; the proven state expects float coordinates already in the
normalized domain.

Each immediate vertex is eight dwords: float4 position, packed diffuse,
packed secondary color, float `s`, float `t`.  The fourth position component
is not padding: it is reciprocal clip W after the draw module's viewport
transform.  Current Mesa writes exactly that value in
`draw_cliptest_tmp.h:176-182` and `draw_vs_variant.c::do_rhw_viewport()`.
The kernel ABI must preserve and validate it rather than synthesizing 1.0.
The old Unichrome driver independently follows the same API contract:
`viaChooseVertexState()` selects `EMIT_4F_VIEWPORT`, `VIA_EMIT_W`, and
`HC_HVPMSK_W` whenever any texture coordinate or fog is present, while it may
use a three-component position only for the untextured/no-fog case.  This is
legacy architectural corroboration, not evidence that its register encoding
can be copied to Chrome9.

For a single triangle use the
already-proven list/Gouraud transition `A20=20000000`, `A23=23004f02`, and
`A24=24000003`.  Gouraud is required because the modulate shader reads
interpolated primary color.

This W requirement is visible in the hardware state too.  Type-02/FE A01
`01002521` decodes texture-0 bits 6:5 as `01`, perspective correction enabled;
bits 4:2 select texture A and bits 1:0 select two-dimensional S/T.  An affine
test where all input clip W values are equal therefore cannot prove the
structured vertex path.  A later ABI must not omit D0.w merely because the
first R/G/B/W texture oracle happens to pass with W=1.

Type-01 A01 independently controls perspective correction for non-texture
interpolants: bit 17 diffuse color, bit 18 specular color, and bit 19 fog.
The Xorg baseline `01c01540` leaves all three clear.  The proprietary Linux GL
driver provides stronger executable evidence than the field table alone:
`stmInitChipImage_inv:0x1787c4-0x1787da` builds subaddress A01 by ORing byte 2
with `ce`, byte 1 with `15`, and byte 0 with `40`, producing `01ce1540` and
therefore enabling all three perspective bits by default.  Use this
vendor-exact A01 word for the first generalized textured path.  If later state
minimization is useful, diffuse-only would be `01c21540`, but that narrower
combination has not been live-qualified.  The `01ce1540` recommendation is
static proprietary-driver evidence, not yet a Chrome9 live result.

The exact `REPLACE` fragment program remains:

```text
90018000 91000001 92010001 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9a000000
TAU 301101b0
ALU 01c00000 301c0000 00f20105 000d083f
```

The vendor-scheduled `MODULATE` controls and RGBA derivative are:

```text
90018000 91000001 92010002 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9a000000
TAU 301001b0
ALU 0140b6d0 30040053 00f20005 0000007f
ALU 01c00000 301c0000 00f20105 000d083f
```

The latter is not yet live-proven.  Its `30040053` high nibble is part of the
sampled source's split W selector; clearing it produces `xyzx`, not RGBA.
Its address-free discriminator and the exact original XRender `wwww` program
are documented with the capture tool in the one-texture oracle.

For the bounded three-vertex ioctl, packet lengths follow mechanically from
the measured quad rather than requiring another guessed terminator.  Keep
dwords 0--159 of the common oracle, change A20/A23/A24 to list/Gouraud/count 3,
and shorten only the final immediate packet:

| program | controls | TAU | ALU packet | immediate packet | total body |
|---|---:|---:|---:|---:|---:|
| REPLACE | `160..179` | `180..187` | `188..195` | `196..223` | 224 dwords |
| MODULATE | `160..179` | `180..187` | `188..199` | `200..227` | 228 dwords |

The immediate packet is four header dwords plus 24 payload dwords for three
eight-dword vertices.  MODULATE's ALU packet is one four-dword header followed
by the two four-dword native instructions.  H6 terminates from A24 plus payload
length; append no Unichrome-style fire word.

The fixed indices below are useful both for construction assertions and for
reviewing relocation safety.  They refer to the 224/228-dword body before any
outer H0 synchronization or CR tail:

| index | state |
|---:|---|
| 4 | destination A50 relocation |
| 20 | source texture A00 relocation |
| 21--25 | texture A18/A20/A21/A22/A30-format |
| 32--34 | proposed explicit sampler A30, then A31/A32 |
| 42, 44 | destination A53 and type-01 A01 |
| 52--56 | type-02/FE A00/A01/A08/A05/A0D |
| 64, 68, 71 | FVF A00, D3 declaration, A62 linkage |
| 72--74 | primitive A20/A23/A24 |
| 132--151 | five native VS instructions |
| 156 | A60 VS-valid/last-index word |

If explicit sampler A30 has not passed its one-delta state-closure test, retain
the exact Xorg contents at indices 32/33 (`A31/A32`) and padding at 34/35;
every later index is unchanged either way.

## Sampler translation

Initially accept only 2D, one layer, first/last level zero, power-of-two width
and height, normalized coordinates, identity/native format swizzles, and
sampler slot zero.  Reject
compare mode, unnormalized coordinates, LOD bias, anisotropy, and border
addressing rather than approximating them.  The exact safe enum mappings are:

| Gallium state | hardware code |
|---|---:|
| nearest min/mag | `0` |
| linear min/mag | `1` |
| no mip filter | `0` |
| nearest mip | `1` |
| linear mip | `2` |
| clamp to edge | `1` |
| repeat | `2` |
| mirrored repeat | `3` |
| mirror once / mirror clamp to edge candidate | `7` |
| clamp to border | `0`, only after A35/A36 are emitted |

Subaddress A31 holds S/T min/mag and mip fields; A32 holds R/T/S wrapping.
Do not expose clamp-to-border while leaving A35/A36 stale.  The exact border
packing and concrete transparent/opaque/half-color words are recovered in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md).  They are type-02
sampler A35/A36 words; do not confuse them with type-01 blend A35/A36.  The
minimal path does not need them.

For slot zero with the vendor's harmless anisotropy-limit field left at 16,
the complete words are:

```text
A30 = 30000000                                # L0 offset/min/max all zero
A31 = 31100000 | (mag << 13) | (min << 10)
               | (mag << 7)  | (min << 4) | mip
A32 = 32000040 | (wrap_t << 3) | wrap_s   # unused R fixed to edge

nearest/no-mip = 31100000
linear/no-mip  = 31102490
repeat S/T     = 32000052
edge S/T       = 32000049
mirror S/T     = 3200005b
```

The measured Xorg packet omits sampler A30 because its initialized image is
already zero.  A persistent DRM submission path should fill one of the two
existing sampler-packet padding dwords with `30000000`; this makes level-zero
selection self-contained without changing packet length.  It is an
address-free, no-op state-closure candidate and should first be checked by
re-running the exact oracle with output unchanged.  Do not rely on stale A30
remaining zero once mip state or multiple contexts exist.

This keeps independently selected minification and magnification filters in
both S and T fields; do not collapse them merely because the first oracle used
nearest for both.

There is no independent linear texture-pitch register, but ordinary alignment
padding does **not** turn A20 into `pitch / cpp`.  Executable Xorg behavior is
exact: `viaSet3DTexture_H5:0x2fc5a..0x2fc88` computes the canonical row as
`align(width * bits_per_pixel, 256) / 8`.  If the supplied pitch equals that
canonical 32-byte-aligned row, it stores the original logical width in A20.
Only when the supplied pitch differs does it store
`pitch / (bits_per_pixel / 8)` as an effective width; A22 then describes that
same substituted width.  The coordinate producer later divides S by the
stored width.

The first truthful sampler path should therefore require
`pitch == align(logical_width * bytes_per_texel, 32)`, emit logical width in
A20/A22, and pass normalized S without any padding scale.  Merely scaling S
for a noncanonical pitch does not implement general REPEAT or CLAMP_TO_EDGE:
the sampler's wrap/clamp boundary would still be the substituted effective
width.  Keep noncanonical imported strides unsupported until an edge-padding
and coordinate/wrap scheme is proven.  Start with level zero.  Mipmaps can follow
using the recovered eight-entry H3 L1--L8 table, but must not be advertised
until all levels are allocated, validated, and tested.

The programming limits make the first ioctl validation concrete.  A20 stores
unsigned 12-bit level-zero height in bits 23:12 and width in bits 11:0, but the
documented maximum of each is 2048.  Require nonzero logical height and
logical width, each at most 2048; require
`pitch == align(logical_width * bytes_per_texel, 32)` and a checked
`pitch * height` within the BO.
For the first power-of-two path set A22 bit 15 and place exact log2(height) in
bits 7:4 and log2(logical_width) in bits 3:0; otherwise clear bit 15 and do
not infer a log from a rounded size.  A00 discards the low eight address bits,
so the source data offset must be 256-byte aligned.  Format `0x98` is manual
class `10011`/subtype `000`, ARGB0888 with implicit alpha 1.0; `0x99` is the
same class/subtype `001`, ARGB8888 with alpha in bits 31:24.  In little-endian
memory these are Gallium B8G8R8X8 and B8G8R8A8 respectively.

## Mesa changes in dependency order

1. Add and probe a `DRAW_TEXTURED_TRIANGLE` kernel capability and matching
   winsys call.  Keep raw command words kernel-private.
2. Accept `PIPE_BIND_SAMPLER_VIEW` for level-zero linear B8G8R8X8 and
   B8G8R8A8 resources, retaining the guard/data-offset convention.  Validate
   view target, format, level/layer range, and swizzle.
3. Store fragment sampler objects instead of discarding them, with reference
   lifetime and slot-zero validation matching sampler views.
4. Classify fragment NIR into exactly three programs: primary color,
   `texture(s0,t0)`, and `texture(s0,t0) * COL0`.  Reject every other texture,
   projection, derivative, compare, offset, gather, or ALU form.
5. Make `chrome9_vbuf_get_vertex_info()` choose six or eight dwords from the
   selected fragment program and request TEXCOORD0 for the textured forms.
   Extend allocation/validation and submission consistently; never accept an
   eight-dword declaration with six-dword storage.  Preserve the emitted
   position[3] reciprocal-W value in D0.w; the present untextured structured
   ioctl discards it.
6. Require one valid bound sampler and view for either textured classifier,
   pass the source handle/state into the structured ioctl, and retain both
   resources through completion.
7. Only then set fragment `max_texture_samplers=max_sampler_views=1`, and set
   the texture/indirection instruction limits consistently with the one-TAU
   implementation.

The first pipe test should render the established red/green/blue/white source
with `REPLACE`, then repeat with white and red primary colors under
`MODULATE`.  It must check every destination pixel, source preservation, all
guards, both BO lifetimes, and the untextured color/D16 regression suite.
Then use a triangle whose three clip-W values differ and whose S/T gradient
has an interior sample that differs between affine and projective
interpolation.  Compare several interior pixels against a CPU barycentric
reference.  Correct output distinguishes preserved D0.w plus A01 perspective
state from the deceptively successful W=1 path without changing addresses or
exercising a new texture format.

Before advertising both filter modes and all three wrap modes, add a texel
center/edge calibration rather than relying only on large uniform quadrants.
Use a canonical-pitch 8x1 POT image with eight distinct texels.  Nearest
samples at `(i+0.5)/8` must select texel i; samples immediately on either side
of every `i/8` boundary recover the tie/bias convention.  Linear samples at
the same centers and boundaries must match a CPU bilinear reference.  Finally
probe `-epsilon` and `1+epsilon` under REPEAT, MIRRORED_REPEAT, and
CLAMP_TO_EDGE.  This catches a half-texel bias or an incorrect logical-width
normalization that the 32x32 R/G/B/W oracle cannot distinguish.

## GL 1.3 is not implied by one sampler

One working unit unblocks context creation and the narrow texture tests; it
does not by itself justify OpenGL 1.3.  The compatibility state tracker can
exercise at least these still-missing driver semantics:

- fixed-function texture environments beyond REPLACE/MODULATE (DECAL, ADD,
  BLEND, COMBINE/DOT3) and GL 1.3 multitexture, which normally requires at
  least two simultaneously usable units;
- mip filtering, texture LOD/ranges, border color, additional base formats,
  cube maps, and compressed/copy texture paths;
- blend factors/equations/constant color, alpha test, channel masks, logic op,
  stencil and two-sided stencil;
- partial scissor, fog, points/lines, polygon modes, and stipple;
- `resource_copy_region`/`blit` behavior used by texture copies and drawable
  management.

The current callbacks for blend color, stencil reference, and polygon stipple
are no-ops, while draw validation rejects most of the corresponding CSOs.
This matters immediately because Mesa 26.2 `main/version.c::compute_version()`
falls back unconditionally to desktop GL 1.3 when the GL 1.4 `ARB_shadow` gate
is false; there is no GL 1.0--1.2 capability ladder.  In addition,
`st_extensions.c` copies `max_tex_instructions` and
`max_tex_indirections` independently of the sampler count, while the current
Chrome9 screen leaves both at zero.  When the one-TAU classifier lands, set
both to one; sampler count alone is internally inconsistent.  Version and
extension exposure must follow state-specific conformance, not Mesa's ability
to construct a compatibility context or its unconditional 1.3 floor.
