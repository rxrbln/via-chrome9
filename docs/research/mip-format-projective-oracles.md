# Chrome9 mip, format, projection, and cache oracles

This note collects the next texture-capability steps into packet-bounded
experiments.  It complements the exhaustive field tables in
[resource-layout-clear-cache.md](resource-layout-clear-cache.md) and
[fixed-function-texture-sync.md](fixed-function-texture-sync.md); the focus
here is the smallest state transition which can be implemented and tested
after the live two-texture path.

The packet fields are cross-checked against the VX900 manual, the proprietary
Linux GL binary, and the Windows D3D/OpenGL binaries.  Old Mesa 7.11.2 is used
only for API lifecycle and a software-projective construction; its UniChrome
register words are not Chrome9 register evidence.  None of the new paths in
this note is promoted to silicon-proven until recorded in
[hardware-lab.md](hardware-lab.md).

## One mip table, exactly

Chrome9 names L0 in the ordinary type-02 image block and L1--L8 in one fixed
eight-entry type-03/subtype-10 table.  For texture stage `n`, the exact table
selector recovered from proprietary Linux
`stmFullUpdateHardware_invi:0x177b4c..0x177bc2` is:

```text
selector = 10030000 | ((n << 3) & 0000ffff)
stage 0  = 10030000
stage 1  = 10030008
```

The table packet is always twelve dwords, even when only L1 is present:

```text
FE03043C 00000008 10030000 CC000000
40000010 00000000 00000000 00000000
00000000 00000000 00000000 00000000
```

This example is stage 0, translated SF memory, and L1 at byte offset `1000`
from L0.  Each entry is `location[31:30] | (relative_byte_offset >> 8)`.
`40000010` must be paired with image A18=`18001101`: bit 12 selects offsets
relative to L0, mode 1 selects a 2D image, and bits 1:0 select SF.  The
apparently similar `00000010` is the SL form and must not be copied into a
translated-SF job.

Both proprietary image builders cap the explicit chain to nine total levels:
L0 plus L1--L8.  Linux does so at `0x1b264a..0x1b2654` and
`0x1b2ab1..0x1b2abb`; Windows does so at
`VTGUModeDX32A:0x101d8112..0x101d811b` and
`0x101d8658..0x101d8665`.  Always emit and initialize all eight entries.
Zero every unused low-24 address rather than retaining a previous resource's
chain.  Do not expose L9--L11 until the implicit L8-rooted tail is measured.

For a POT linear uncompressed level, the vendor allocation rule is:

```text
pitch_l = align(max(width0 >> l, 1) * cpp, 32)
size_l  = pitch_l * align(max(height0 >> l, 1), 8)
offset_l = sum(size_0 .. size_(l-1))
```

Each contribution is consequently 256-byte aligned.  Derive these offsets
in the kernel from dimensions and a known format; do not accept raw table
entries from userspace.  The image and every reachable level remain one
reserved BO behind the same leading/trailing guards.

In a one-texture three-vertex body, insert the table after the image block and
before the sampler.  If `B=4` for the closed destination/blend tail, the table
header is body `28+B`, the first entry is `32+B`, and every word from the
sampler onward moves by twelve.  Exact resulting lengths are:

```text
REPLACE:  236+B dwords
MODULATE: 240+B dwords
```

For the live two-texture triangle, one stage-0 table changes the 262-dword
body to `274+B`.  Its sampler-0 begins at `40+B`, image-1 at `48+B`, RGBA
source-C remains `30202053` but moves to `229+B`, and the immediate header
moves to `240+B`.  A second table with selector `10030008` changes it to
`286+B`.

## Exact LOD word and nonzero-level gates

The same A30 subaddress is reused in different type-02 groups.  The image
A30 contains the format byte; the sampler A30 contains LOD state.  The
sampler builder is byte-identical in Windows D3D and OpenGL at
`0x101d9a40` / `0x10242260`, and matches proprietary Linux
`STM_MipMapLOD_INV:0x1b1a50`:

```text
sampler_A30 = 30000000
            | ((signed_lod_bias_s5_5 & 03ff) << 12)
            | ((maximum_level & 3f) << 6)
            |  (minimum_level & 3f)
```

Old Mesa independently converts a floating bias by truncating `bias*32` and
clamping it to `[-512,511]` (`via_state.c:66..76`).  That is a semantic
cross-check for signed 5.5 conversion, not evidence for its older LOD
register layout.

Use two different gates because they prove different mechanisms:

1. **Address L1 without derivatives.**  Keep A31=`31100000`, set
   A30=`30020040` (+1.0 bias, allowed L0--L1), and use the L1 table above.
   With red L0 and green L1, the result must be green.  This first test
   isolates the table address and A30 offset without enabling mip filtering.
2. **Exercise level bounds and mip selection.**  Set A31=`31100001`
   (nearest mip) and select each physical level by making min=max.  The exact
   A30 sequence for L0--L8 is:

```text
30000000 30000041 30000082 300000C3 30000104
30000145 30000186 300001C7 30000208
```

Fill the nine levels with distinct constant colors.  This checks every table
entry and catches swapped min/max fields without relying on a screen-space
derivative.  Only after both gates pass use ordinary derivative selection:

```text
nearest, L0 only             30000000 31100000 32000052
bilinear, L0 only            30000000 31102490 32000052
bilinear + nearest mip L0-L1 30000040 31102491 32000052
trilinear L0-L1              30000040 31102492 32000052
```

Transition closure is mandatory.  Follow forced L1 immediately with the same
resource using sampler A30=`30000000`, then force L1 again.  The middle draw
must return red.  Leaving an old table resident is harmless only while the
new A30 makes every nonzero level unreachable; therefore every sampler packet
still writes A30, A31, and A32.

### A nonzero one-level view can be rebased

For a Gallium view whose `first_level == last_level == k`, the smallest safe
implementation does not need to expose physical level `k` through the mip
table.  Validate the vendor-derived byte offset, relocate the image base to
that level, program A20/A22 from that level's dimensions, and treat it as a
new single-level image with sampler A30=`30000000`.  The BO reservation and
guards still cover the complete parent resource.

This rebase is equivalent only for a one-level view.  A range `k..m` must
rebuild all table offsets relative to rebased level `k`, use min zero and max
`m-k`, and revalidate every derived address.  It must not retain the parent
L0 dimensions or absolute hardware level numbers.  Old Mesa's
`viaSetTexImages()` independently recomputes `firstLevel`, `lastLevel`, and
residency when base/min/max LOD changes (`via_tex.c:481..515`), supporting
that lifecycle without supplying Chrome9 packet words.

## First additional texture formats

The Windows public-format converter at `VTGUModeDX32A:0x100c7590` and sampler
lookup at `0x1020a660` (OpenGL copy `VTGOGL32A:0x1025d590`) independently
close the following public chains; the old Mesa switch at
`via_tex.c:445..475` independently confirms their API-level use on VIA
hardware:

| format | image A30 | cpp / 32-wide pitch | direct endpoint source |
|---|---:|---:|---|
| R5G6B5 | `30890000` | 2 / 64 | `f800,07e0,001f,ffff` |
| X1R5G5B5 | `30880000` | 2 / 64 | `7c00,03e0,001f,7fff` |
| A1R5G5B5 | `308A0000` | 2 / 64 | `fc00,83e0,801f,ffff` |
| A4R4G4B4 | `308B0000` | 2 / 64 | `ff00,f0f0,f00f,ffff` |
| L8 | `30130000` | 1 / 32 | `00,ff` |
| A8 | `301B0000` | 1 / 32 | `00,ff` alpha |
| A4L4 | `30140000` | 1 / 32 | `f0,ff,0f` |
| A8L8 | `30150000` | 2 / 64 | `ff00,ffff,00ff` |
| L16 | `30160000` | 2 / 64 | `0000,ffff` |

For each direct decoder gate, keep 32x32 POT geometry, nearest filtering,
clamp, one level, and the already-live REPLACE program.  Only image A30,
bytes-per-texel, canonical pitch, useful range, and source contents change.
R5G6B5 is the first gate: its red/green/blue/white endpoints must produce the
same XRGB quadrants as BGRA, while a stale 32-bpp interpretation reads mapped
poison instead.  The full 16-bit useful range is `0x800`; for one-byte formats
it is `0x400`.

Do not infer stored alpha from an XRGB REPLACE result.  On live format-99
destinations use the already-proven source-over equations and extreme alpha
values.  For A1R5G5B5, `7c00/fc00` red over opaque green must produce
unchanged green/red.  For A4R4G4B4, use `0f00/ff00`.  For A8, black with
alpha `00/ff` over white must produce white/black.  A4L4 bytes
`f0/ff/0f` mean opaque black/opaque white/transparent white; A8L8 words
`ff00/ffff/00ff` mean the same.  L8 and L16 instead supply alpha one and map
their zero/full endpoints to black/white.

Run each format as an `old -> new -> old` transition without context reset.
The last draw must reproduce the original BGRA/XRGB result.  This catches a
format byte which was validated in software but not actually rewritten at
body `25+B` for the one-texture packet or `25+B/45+B` for two textures.

## Perspective and projective coordinates are different features

The existing packet already enables perspective-correct interpolation of S/T:

```text
type-02/FE A01 = 01002521
stage-0 bits 6:5 = 01       perspective correction
stage-0 bits 4:2 = 000      source texture coordinate A
stage-0 bits 1:0 = 01       two coordinates, S/T
```

Together with finite reciprocal clip-W in D0.w, this path is live.  It is not
projective texturing by an independent texture Q.  For ordinary perspective,
the rasterizer computes the usual
`sum(lambda*s*rhw) / sum(lambda*rhw)`.

The proprietary Linux driver contains a real native projection path.
`STM_TexTransformFlag_H5:0x1b1900..0x1b1a41` and
`__SetupTexProjection_invi:0x1b1f70..0x1b20c0` both clear or set bit 11 in
the image record's A18 word.  Thus its exact translated-SF image transition
is:

```text
ordinary 2D:       A18 = 18001101
projection class:  A18 = 18001901
                              ^ bit 11
```

The same routines mark that stage projected in the driver's general-texture
state.  The manual requires a four-coordinate stage and defines the native
stage-0 field as perspective-mode `10`, dimension `11`, source A, i.e. low
field `43` instead of ordinary `21`.  This yields the field-derived native
candidate A01=`01002543`.  However, the executable path constructs that word
through an internal compact state image rather than an immediate literal,
and no captured native-projective packet or silicon result yet independently
checks the complete word.  Consequently `18001901/01002543` is a guarded
native oracle, not a public ABI recipe.

### Safe first projective implementation

Old Mesa implements the lower-risk one-texture solution exactly in
`via_tris.c:140..149, 787..845`.  For each vertex with texture
`(s,t,r,q)` and position reciprocal W `rhw`, it removes Q from the hardware
vertex and submits:

```text
rhw' = rhw * q
s'   = s / q
t'   = t / q
```

The unchanged `01002521` perspective path then computes:

```text
sum(lambda*s'*rhw') / sum(lambda*rhw')
 = sum(lambda*s*rhw) / sum(lambda*q*rhw)
```

which is the required projective S/Q result (and likewise T/Q).  This is a
software vertex transform, not a register guess.  It fits the current
software-T&L Gallium design and adds no command words.  Accept it initially
only for one active texture, finite nonzero Q, and finite transformed values.
Reject two independently projected texture units because one D0.w cannot
carry two different Q denominators; old Mesa makes the same fallback choice
at `via_tris.c:835..842`.

The proprietary Linux helper
`stmMTRAdjustTriangleTexCoordsProjected:0x000fdbb0..0x000fdc0c`
independently divides corresponding coordinate arrays by one supplied Q.  It
does not by itself prove a hardware word, but it confirms that the shipping
Chrome9 stack also used software coordinate adjustment around its native
projection state rather than assuming every projective case was transparent.

A bounded discriminator uses an 8x1 distinct-color texture and equal clip-W
for all vertices.  Pick Q=`1` at v0 and Q=`4` at v1/v2, with S=`0,1,1`.
Choose an interior pixel whose affine S and projective S/Q land in different
texels, verify several pixels away from edges, and immediately follow with an
ordinary Q=1 draw.  The ordinary draw must still use the original reciprocal
W and coordinates.  Test the native A18/A01 candidate only after this
software construction is live and only behind a diagnostic capability.

## Cache and ownership closure

Five similarly named controls are not interchangeable:

| state | exact role | initial policy |
|---|---|---|
| type-01 A01 bit 8 | texture-cache enable; zero also clears | keep enabled |
| type-02/FE A00 bit 0 | one-shot texture-cache clear | set for every texture job |
| image A18 bit 23 | force every lookup to miss | diagnosis only |
| SF PTE bit 25 | cached/snooped system page | match GEM shmem mapping |
| SR6F bit 7 | publish GTI page-table changes | only for mapping changes |

The exact one-stage general group uses A00=`00000011`: one active texture in
`[7:4]` plus clear bit zero.  Two stages use `00000021`.  The same one-stage
group with no explicit clear is `00000010`.  A force-miss diagnostic changes
ordinary SF A18 from `18001101` to `18801101`.

Neither proprietary driver hides a missing GPU barrier behind a generic
helper: `stmFlushPipe_inv:0x176260` returns without emitting work,
`stmUpdateTextureUsageEventTag_inv:0x1767c0` is empty, and
`STM_TextureMap_INV:0x1b2df0` binds state without waiting.  CPU cache flush,
GTI publication, producer retirement, and the FE A00 texture-cache pulse are
therefore four separate obligations.

For CPU upload -> sample, flush the full source mapping, perform the DMA
ownership transition, then publish a packet with FE A00 bit 0.  For render ->
sample, keep the BO reserved, order after the producer fence, and emit the
same FE clear in the consumer.  Do not CPU-flush a device-written BO as a
substitute.  Diagnose suspected stale texture data with this three-way
comparison only:

```text
normal clear:  FE A00=00000011, A18=18001101
no clear:      FE A00=00000010, A18=18001101
forced miss:   FE A00=00000010, A18=18801101
```

If forced miss sees new data while the normal clear does not, stop exposing
render-to-texture and investigate pulse ordering.  Do not ship force-miss as
the permanent workaround.

## Ordered enablement

1. Forced L1 with `30020040`, one SF table entry, nearest/no-mip.
2. L0--L8 fixed-level sweep with nearest-mip and min=max.
3. One-level nonzero view rebased as a new L0.
4. R5G6B5 direct REPLACE, then A1R5G5B5/A4R4G4B4 RGB and alpha gates.
5. L8/L16, then the stored-alpha luminance formats.
6. Bilinear nearest-mip and trilinear derivative cases.
7. Software-projective one-texture transform.
8. Render-to-texture clear/no-clear/force-miss discriminator.
9. Native `18001901/01002543` projection only as a quarantined diagnostic.

Every stage is an enable/disable/enable transition in one context.  Retain
the target and every reachable mip level through the completion fence, check
all useful bytes and guards after retirement, and quarantine every mapping on
a timeout.  A failed table, format, or projection experiment is never
recovered by publishing a following closure draw.
