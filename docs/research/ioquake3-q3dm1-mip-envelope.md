# q3dm1 mip envelope and first game-capable Chrome9 successor

Status: exact ioquake3 source and shipped-asset audit plus static Chrome9
packet proof, 2026-09-21.  The L1--L8 register encoding is independently
cross-checked in the manual and three proprietary builders.  The current
compatibility-guarded L0--L8 layout and derivative mip fetch have not yet run
on VX900, so the implementation order below remains mandatory.

## Immediate result

The first normal q3 draw after the newly recognized packed lightmap program
is blocked by sampler state, not by another fragment program.  ioquake3's
`GL_LINEAR_MIPMAP_NEAREST` becomes Gallium linear minification, linear
magnification, and `PIPE_TEX_MIPFILTER_NEAREST`.  Current
`chrome9_create_sampler_state()` accepts only `PIPE_TEX_MIPFILTER_NONE`, sets
the sampler object's `supported` member false, and
`chrome9_draw_state_supported()` returns with `reason=sampler-state` before
any resource check or ioctl.

For the packed-lightmap draw the source-predicted diagnostic is:

```text
reason=sampler-state fs=24 layout=1 tex=2 views=2 samplers=2
```

This is not quoted from a retained post-fix run.  The retained normal run
proves the selected texture mode, while program 24 was captured under the
diagnostic non-mip `GL_LINEAR` mode.  A fresh run must archive the actual
line.  Current fragment-program numbering ends at packed generic 23 and
packed lightmap 24.  The 1,304-bind semantic trace contains no third
unsupported object and gives no evidence for a program 25.

Merely accepting the Gallium mip enum would be incorrect.  The current
two-texture UAPI has no mip metadata, Mesa copies no mip filter, the kernel
writes sampler A30=`30000000`, leaves A31's mip field zero, and emits no H3
level table.  Such a change would silently fetch L0 instead of implementing
the requested filter.

## The shipped map needs only explicit L1--L8 at defaults

The input is `/tmp/pak0.pk3`, SHA-256
`e77abad2466f45a0a7ea018445528f9b95a0fe7789fa1abc1a7718bbf0754b08`.
Its `maps/q3dm1.bsp`, SHA-256
`3d2dad8e6ba05914fc90619a6517ecdee7e5eb50f86013a14a84bb3c61cd702e`,
names 94 BSP shaders.  Resolving those names through every shipped
`scripts/*.shader`, including `map`, `clampmap`, and `animMap`, found 104
distinct referenced image files.  `noshader` and the absent
`textures/gothic_door/km_arena1archfinalc_mid` are the only unresolved image
names; neither can raise the measured maximum.

The two largest source images are:

| image | source dimensions | SHA-256 |
|---|---:|---|
| `textures/gothic_door/km_arena1columna2R.jpg` | 66x640 | `5fb9db7902440b95da21184317161bd5f6f664de38540a6fe0271aacc3232188` |
| `textures/gothic_floor/largerblock3b.jpg` | 512x512 | `a0bf109cf82c10316063aa20a93cc3e86ac6df0ad928a8a39dde8e7b34f57fc7` |

ioquake3 GL1 `Upload32()` first rounds each axis down to a power of two when
the default `r_roundImagesDown=1` is active, then right-shifts both axes by
the default `r_picmip=1` for `IMGFLAG_PICMIP`.  Shader stage `map`,
`clampmap`, and `animMap` images receive both `IMGFLAG_MIPMAP` and
`IMGFLAG_PICMIP` unless the shader declares `nomipmaps` or `nopicmip`; none
of the resolved q3dm1 definitions does.  Raw images loaded without a shader
use the same flags.

Consequently the two maxima upload as 32x256 and 256x256.  Across the 104
resolved assets the resulting maximum-level distribution is:

| last level | distinct images |
|---:|---:|
| 8 | 2 |
| 7 | 74 |
| 6 | 17 |
| 5 | 10 |
| 2 | 1 |

Thus the default q3dm1 base textures need at most L0--L8.  Every address is
present in the explicit eight-entry H3 table; the unproven implicit L9--L11
tail is not a default-map prerequisite.  This is an application envelope,
not a reason to advertise an eight-level GL limit globally.

The distinction matters for testing.  With diagnostic `r_picmip=0`, both
large images reach maximum dimension 512 and require L9.  Keep that mode
disabled until the separate implicit-tail oracle succeeds, or retain the
existing fail-closed view rejection.  The BSP also contains nine 128x128
lightmaps.  `tr_bsp.c` creates them with CLAMP_TO_EDGE and without
`IMGFLAG_MIPMAP`, so unit 1 remains a single-level sampler in the ordinary
world draw.

## Exact default world tuple

The first packed-lightmap mip successor needs this closed state:

```text
fragment program:  RGB = tex0 * tex1 * primary
alpha:             primary or tex0 * primary, according to translated view
destination/depth: XRGB + D16 LEQUAL, writes enabled
blend/alpha test:  disabled
unit 0:            REPEAT S/T, linear min/mag, nearest mip, L0--Llast
unit 1:            CLAMP S/T, linear min/mag, no mip, L0 only
```

For a maximum L8 base chain the semantic sampler words are exact:

```text
unit 0 A30 = 30000200       # min 0, max 8, zero bias
unit 0 A31 = 31102491       # linear spatial, nearest mip
unit 0 A32 = 32000052       # repeat S/T

unit 1 A30 = 30000000       # L0 only
unit 1 A31 = 31102490       # linear spatial, no mip
unit 1 A32 = 32000049       # clamp S/T
```

Program 24 is a Mesa classifier number, not a native/UAPI program enum.  It
lowers to the full-primary or tex0-alpha-primary three-factor object according
to the translated sampler formats.  Keep that semantic selection independent
from mip state.

## Current guarded L0--L8 image

Do not copy the vendor-packed offsets into the current Mesa allocation.  Its
compatibility layout preserves the old complete L0 object: one leading page,
L0 rounded to a page, an internal L0 guard page, 256-byte-aligned successor
levels, and one final guard page.  The H3 entries are SF-relative offsets from
the L0 base, not object-relative offsets.

For the default maximum 256x256 BGRX/BGRA chain the exact current layout is:

| level | dimensions | object offset | L0-relative offset | pitch/bytes | H3 word |
|---:|---:|---:|---:|---:|---:|
| L0 | 256x256 | `01000` | `00000` | `400/40000` | image A00 |
| L1 | 128x128 | `42000` | `41000` | `200/10000` | `40000410` |
| L2 | 64x64 | `52000` | `51000` | `100/04000` | `40000510` |
| L3 | 32x32 | `56000` | `55000` | `080/01000` | `40000550` |
| L4 | 16x16 | `57000` | `56000` | `040/00400` | `40000560` |
| L5 | 8x8 | `57400` | `56400` | `020/00100` | `40000564` |
| L6 | 4x4 | `57500` | `56500` | `020/00080` | `40000565` |
| L7 | 2x2 | `57600` | `56600` | `020/00040` | `40000566` |
| L8 | 1x1 | `57700` | `56700` | `020/00020` | `40000567` |

The complete allocation is `0x59000` bytes.  `0x0000..0x0fff` is the leading
guard, `0x41000..0x41fff` is the retained L0 guard, and
`0x58000..0x58fff` is the final guard.  The corresponding state is:

```text
FE020440 0000043C 00020000 CC000000
<L0-SF>>8 18001101 20100100 21000001
22008088 30980000 CC000000 CC000000

FE03043C 00000008 10030000 CC000000
40000410 40000510 40000550 40000560
40000564 40000565 40000566 40000567
```

The asymmetric 32x256 case is also maximum-level eight.  Its H3 entries are
`40000090,400000b0,400000b8,400000bc,400000be,400000bf,400000c0,400000c1`
and the complete current allocation is `0xf000` bytes.  Testing both shapes
prevents a square-image-only address calculation from passing accidentally.

## Smallest useful successor

The two-level one-texture ioctl remains the safest first silicon gate, but it
cannot make the normal q3 world path correct.  After it passes, extend the
same fixed 12-dword H3 packet to L0--L8 and add a versioned two-texture+D16
successor whose semantic additions are only:

```text
texture 0 last_level: 1..8
texture 0 mip_filter: NONE or NEAREST initially
texture 0 lod_bias_s5_5: zero initially
texture 1: exact existing single-level contract
```

Do not expose raw offsets, pitches, table words, or GPU addresses.  The kernel
derives every level using the capability-defined current guarded layout,
requires the exact complete BO size, verifies all three guards, initializes
all eight H3 entries, and retains the whole chain through retirement or
timeout quarantine.  Keep `first_level=0`; a view-range ABI comes later.

Insert selector `10030000` immediately after unit 0's image packet and before
unit 0's sampler packet.  It adds exactly 12 dwords regardless of level count.
Against the current one-triangle two-texture builder:

| native object | D16 body before mip | with unit-0 H3 table |
|---|---:|---:|
| two-factor | 266 | 278 |
| full primary | 270 | 282 |
| tex0-alpha primary | 274 | 286 |

Blend adds the existing four-dword delta orthogonally.  Rewrite unit 0's
complete table and A30/A31/A32 on every draw; rewrite unit 1's complete
single-level state too.  The old path remains safe after a mip draw only
because it restores A30/A31 so no stale successor entry is reachable.

## Bounded promotion sequence

1. Run the existing guarded 32x32/16x16 one-texture address, forced-level,
   derivative-nearest, and transition oracle.
2. Allocate the exact 256x256 current-layout chain above.  Give L0--L8 nine
   distinct solid colors and force `min=max=level` with A30 words
   `30000000,30000041,30000082,300000c3,30000104,30000145,30000186,`
   `300001c7,30000208`.  This proves every explicit address without relying
   on derivatives.
3. Repeat the forced sweep on 32x256.  Verify every row/padding byte and all
   guards after each draw.
4. Add an opaque-white single-level unit 1 and D16 LEQUAL/write, initially
   with unit-0 coordinates inside `[0,1]`.  Run both program-24 alpha forms
   and old/new transition closure.
5. Set A30/A31=`30000200/31102491`; minify 256 to 128 and require L1, then
   draw 1:1 and require L0.  Repeat with the now-live signed REPEAT coordinate
   sweep through `+/-31.109375`.
6. Only after pixels, all source/depth/destination guards, retirement cookie,
   CR PAUSE, and work/error bits are clean should Mesa accept
   `PIPE_TEX_MIPFILTER_NEAREST` for this exact capability tuple.
7. Rerun q3dm1 at defaults and archive the actual first new rejection.  Keep
   `r_picmip=0`, mip-linear, L9--L11, nonzero bias, nonzero first level, and
   imported/noncanonical chains rejected until their separate oracles pass.

This sequence avoids conflating a shader success, H3 address success,
derivative LOD success, and the full four-object q3 state in one first live
packet.
