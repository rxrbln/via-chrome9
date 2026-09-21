# Chrome9 one/two-texture D16 blend and alpha-test ABI

Status: implementation-grade static design, 2026-09-21. The underlying
one-texture, two-texture, D16, blend, alpha-test, BGRA, scissor and four-BO
lifetime pieces have independent live coverage. The combined public ioctls
defined here are not implemented or advertised yet and require the staged
silicon gates below.

## Scope and evidence

This successor combines the existing normalized
`drm_chrome9_blend_state` with the established one- and two-texture D16
draws. “Arbitrary blend” here means every ADD equation composed from the
current ZERO, ONE, source/destination color/alpha, inverse, and
SRC_ALPHA_SATURATE factors, independent RGB/alpha factors, any color mask,
and an independent fixed alpha test. Factors above INV_SRC_ALPHA require the
live `CAP_BLEND_EXTENDED_FACTORS` bit 29. It does **not** silently add
constant factors or non-ADD operations.

The native state is cross-checked independently by:

* Linux 5.75 `via_chrome9_dri.so` (SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`),
  symbols `STM_AlphaTest_INV:0x194c40`, `STM_AlphaFunc_INV:0x194ad0`,
  `STM_AlphaBlend_INV:0x196e30`, and the depth builders;
* Windows D3D `VTGUModeDX32A.dll` (SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`),
  alpha enable/function `0x101fab20/0x101facf0`, the state builder at
  `0x101fb090`, blend at `0x101fd9e0`, and D16 state at
  `0x101fa910/0x101faa10`;
* Windows OpenGL `VTGOGL32A.dll` (SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`),
  whose depth, alpha and blend builders at `0x1024e6d0`, `0x1024edc0`, and
  `0x10251220` are byte-equivalent to the D3D implementations; and
* Mesa 7.11.2 UniChrome `via_state.c`, which independently maps the GL
  compare/factor API and disables alpha test separately. It is a semantic
  older-generation cross-check, not the Chrome9 packet source.

The Windows pair proves that A33 is ordinary subaddressed state: neither
implementation imposes an ordering dependency between A33 and A10--A20.
The live raw two-texture alpha+D16 discriminator documented in
`fixed-function-state-transitions.md` proves the compact body-19 placement
on VX900 silicon.

## Why new ioctls are required

The existing one-texture D16 request has only three reserved dwords and the
two-texture request has five. A normalized blend state is ten dwords. Reusing
the old requests would either make reserved nonzero data acquire meaning or
leave part of the blend state implicit. Both break the existing ABI.

Reserve two new private commands and two capability bits from the canonical
allocation ledger:

```text
DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_D16_BLEND      0x12
DRM_CHROME9_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND  0x13

DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_D16_BLEND      (1ULL << 27)
DRM_CHROME9_CAP_DRAW_TWO_TEXTURED_TRIANGLE_D16_BLEND  (1ULL << 28)
```

The associated x86 ioctl values are `0x40e86452` for the 232-byte
one-texture request and `0x41286453` for the 296-byte two-texture request.
All members are fixed-width integers or fixed-width nested records, so the
layout is identical for 32- and 64-bit userspace.

The capabilities are independent of the old canonical-source-over shortcut.
Require the new per-draw capability plus `CAP_ALPHA_TEST` when alpha test is
enabled. For a BGRA destination also require the already advertised
`CAP_TEXTURED_D16_BGRA8888` and `CAP_BLEND_BGRA8888`. BGRX does not need
either BGRA cap.

## Exact UAPI records

The one-texture record deliberately preserves the scalar field order of
`drm_chrome9_draw_textured_triangle_d16`, inserting only the complete blend
state before the vertices:

```c
struct drm_chrome9_draw_textured_triangle_d16_blend {
        __u32 destination_handle;       /* 0 */
        __u32 texture_handle;           /* 4 */
        __u32 depth_handle;             /* 8 */
        __u32 flags;                    /* 12: DEPTH_FUNC | READ_ONLY */
        __u32 program;                  /* 16: REPLACE or MODULATE */
        __u32 width;
        __u32 height;
        __u32 destination_pitch;
        __u32 depth_pitch;
        __u32 texture_width;
        __u32 texture_height;
        __u32 texture_pitch;
        __u32 texture_format;
        __u32 min_filter;
        __u32 mag_filter;
        __u32 wrap_s;
        __u32 wrap_t;                   /* 64 */
        struct drm_chrome9_blend_state blend; /* 68, size 40 */
        struct drm_chrome9_textured_vertex vertices[3]; /* 108 */
        struct drm_chrome9_scissor_state scissor;       /* 204 */
        __u32 destination_format;       /* 216 */
        __u32 reserved[3];              /* 220, must be zero */
};                                     /* size 232 */
```

The two-texture record preserves the existing texture-state and vertex
records. It replaces one old reserved dword with destination format and adds
the blend state:

```c
struct drm_chrome9_draw_two_textured_triangle_d16_blend {
        __u32 destination_handle;       /* 0 */
        __u32 depth_handle;             /* 4 */
        __u32 flags;                    /* 8: DEPTH_FUNC | READ_ONLY */
        __u32 width;
        __u32 height;
        __u32 destination_pitch;
        __u32 depth_pitch;
        __u32 reserved0;                /* 28, must be zero */
        struct drm_chrome9_texture_state textures[2]; /* 32 */
        struct drm_chrome9_blend_state blend;         /* 104 */
        struct drm_chrome9_two_textured_vertex vertices[3]; /* 144 */
        struct drm_chrome9_scissor_state scissor;     /* 264 */
        __u32 destination_format;       /* 276 */
        __u32 reserved[4];              /* 280, must be zero */
};                                     /* size 296 */
```

Both accept the depth-function field plus live `DEPTH_READ_ONLY` bit 11 when
capability bit 23 is present. Unlike the older D16 requests, these state-complete
successors require an explicit nonzero compare field; the first implementation
accepts LESS and LEQUAL exactly as the live D16 predecessor does. Bit zero of
the old request
(`TEXTURED_D16_SOURCE_OVER`) is invalid here: source-over is spelled out in
`blend`, and accepting both encodings would create two sources of truth.
EQUAL and the remaining compares remain later capability-gated extensions.

## Normalized validation

Validation completes before GEM lookup or packet construction:

1. Reject all `flags` bits outside `DEPTH_FUNC_MASK|DEPTH_READ_ONLY`, a zero
   compare field, and every compare except LESS/LEQUAL. The read-only bit is
   accepted only with cap 23. Require every reserved field to be zero. The
   generic zero-means-LESS rule remains only for older D16 requests.
2. Reuse `chrome9_validate_blend_state()`: flags may contain only BLEND and
   ALPHA_TEST; factors `0..3` are baseline, while `4..10` require live cap 29;
   both functions must be ADD; color mask is four bits; alpha function is
   `0..7`, reference is `0..255`; disabled features carry their canonical
   zero/COPY dependents.
3. Validate destination format BGRX/BGRA, dimensions `1..2048`, canonical
   `ALIGN(width*4,32)` color pitch, canonical `ALIGN(width*2,32)` D16 pitch,
   and the half-open scissor.
4. For each texture require dimensions `1..2048`, canonical
   `ALIGN(width*4,32)` pitch, BGRX/BGRA, NEAREST/LINEAR and the three proven
   wraps. One texture accepts only REPLACE/MODULATE; two textures are fixed
   RGBA-by-RGBA MODULATE.
5. Require three distinct nonzero handles for one texture and four for two.
   Reject true imported proxies or foreign-device objects. A same-device
   dma-buf round trip returns the original GEM object and is therefore valid.
   Target and D16 allocation sizes are exact canonical guarded sizes. A texture
   may be larger only under the existing level-zero mip-allocation rule; its
   canonical L0 extent and both applicable guards remain validated.
6. Validate every X/Y against the target, Z in `[0,1]` 16.16, finite positive
   RHW, finite/safe S/T for every active unit, and a nondegenerate triangle.
   Packed color/specular are data, not native command words.
7. Check every multiply/add for overflow and keep total mapped size below
   `SZ_64M - CHROME9_RING_SIZE`.

Return `-EINVAL` for malformed fields/layout/aliasing, `-EOPNOTSUPP` for a
well-formed but unsupported blend enum, `-ENOENT` for a missing handle, and
`-E2BIG` for overflow or aperture excess. The kernel never accepts A00,
A33--A3F, A52/A53, or any header from userspace.

Mesa additionally requires that the closed fragment key produces a defined
alpha before enabling fixed A33. For the current bounded programs REPLACE,
RGBA MODULATE and RGBA-by-RGBA MODULATE satisfy this. An arbitrary NIR shader
does not become valid merely because the kernel can alpha-test the store.

## Native lowering and A33 collision closure

The state-complete successor always selects the expanded destination block,
even for blend-disabled alpha-only or BGRA draws. Construct:

```text
A38..A3F = 38000000 39000000 3a000000 3b000000
            3c000000 3d000000 3e000000 3f000000
A34..A37 = chrome9_lower_blend(normalized factors/functions)
A53       = 53010c00 | hardware_color_mask
A00       = 00000001 | (blend ? 00000010 : 0)
                         | (alpha_test ? 00100000 : 0)
A33       = alpha_test ? 33000000 | func<<12 | encode_ref(ref)
                       : 33007000
encode_ref(ff) = 400; encode_ref(x) = (x<<2) | (x>>6)
```

The live factor codes are closed and exact:

| normalized factor | RGB code | alpha code |
|---|---:|---:|
| ZERO | `05` | `09` |
| ONE | `15` | `19` |
| SRC_ALPHA | `02` | `01` |
| INV_SRC_ALPHA | `12` | `11` |
| SRC_COLOR | `00` | `01` |
| INV_SRC_COLOR | `10` | `11` |
| DST_ALPHA | `03` | `02` |
| INV_DST_ALPHA | `13` | `12` |
| DST_COLOR | `01` | `02` |
| INV_DST_COLOR | `11` | `12` |
| SRC_ALPHA_SATURATE | `08` | `19` |

Pack them as `A34=34000000|(rgb_src<<4)`,
`A35=35000100|(rgb_dst<<2)`, `A36=36000400|(alpha_src<<4)`, and
`A37=37000200|(alpha_dst<<2)`. A53 maps API R/G/B/A mask bits to hardware
bits 2/1/0/3 respectively. These transformations stay kernel-owned.

Always emitting disabled A33 as ALWAYS/ref-zero closes transitions without
affecting fragments because A00 bit 20 is clear. A52 is
`52980000|pitch/32` for BGRX and `52990000|pitch/32` for BGRA.

The current one-texture builder first puts A33 in `common[7]`, then D16
overwrites `common[7]` with A10. Do not lengthen that common packet. Make the
destination payload 16 dwords and put A33 at body 19, the alignment word
already available after the 15 destination/blend words. It is a complete
self-addressed type-01 word written before A00 later enables it.

No A7B word belongs in either draw. Type-01 A7B is reserved fog state in the
VX900 manual; HParaType-10 A7B is global command-queue length on the related
HCM engine; VGA SR7B is a third register namespace owned by engine init.

## One-texture packet map

Define `D=4` for the D16 expansion and `B=4` for the closed destination
expansion relative to the old one-texture body. Both are mandatory here.
The body is 232 dwords for REPLACE and 236 for MODULATE:

```text
body[0..3] = fe020440 0000043c 00010000 cccccccc
```

| dwords | exact block |
|---:|---|
| `0..19` | destination H2, A50/A51/A52, A38--A3F, A34--A37, A33 |
| `20..31` | texture-0 image H2 and state; base at 24, A30 at 29 |
| `32..39` | texture-0 sampler H2 and state |
| `40..55` | common H2 and A86,A87,A53,A00,A01,A88,A89,A10--A13,A20 |
| `56..67` | general one-texture state |
| `68..99` | H6 FVF/linkage; list A20/A23/A24 at `80/81/82` |
| `100..107` | PS constants |
| `108..127` | VS float/integer constants |
| `128..135` | clip A80--A83 |
| `136..159` | five-instruction VS |
| `160..167` | VS activation |
| `168..187` | one-texture PS controls |
| `188..195` | one TAU |
| `196..203` or `196..207` | REPLACE or MODULATE ALUs |
| `204..231` or `208..235` | immediate H2 plus three eight-dword vertices |

The exact state indices are:

```text
body[6]       A52 destination format/pitch
body[7..14]   A38..A3F, all zero
body[15..18]  A34..A37
body[19]      A33 (or canonical disabled A33)
body[46]      A53
body[47]      A00
body[48]      A01 = 01c0154c/548 REPLACE, 01ce154c/548 MODULATE
body[51..55]  A10,A11,A12,A13,A20
```

The first A01 value in each pair is write-enabled; the `...548` form is
read-only. Packet length and D16 object lifetime do not change.

A13 is `13010000` for LESS or `13030000` for LEQUAL. The packet assertions
must test both total lengths and all these fixed indices before publication.

## Two-texture packet map

The same mandatory `B=4,D=4` growth turns the established 262-dword body
into exactly 270 dwords:

```text
body[0..3] = fe020440 0000043c 00010000 cccccccc
```

| dwords | exact block |
|---:|---|
| `0..19` | closed destination/blend state and A33 |
| `20..31` | texture image 0; base at 24, A30 at 29 |
| `32..39` | sampler 0 |
| `40..51` | texture image 1; base at 44, A30 at 49 |
| `52..59` | sampler 1 |
| `60..75` | common render/depth state |
| `76..87` | general two-texture state |
| `88..119` | H6 FVF/linkage; list A20/A23/A24 at `101/102/103` |
| `120..127` | PS constants |
| `128..147` | VS constants |
| `148..155` | clip |
| `156..183` | six-instruction VS |
| `184..191` | VS activation |
| `192..211` | two-texture PS controls |
| `212..219` | two TAUs |
| `220..233` | RGBA product ALUs/terminators; source-C word at 225 |
| `234..235` | alignment |
| `236..269` | immediate H2 plus three ten-dword vertices |

Its common group is exact:

```text
body[64..70]  A86,A87,A53,A00,A01,A88,A89
body[71..75]  A10,A11,A12,A13,A20
A01           01c0154c write, 01c01548 read-only
```

The destination words use the same indices 6, 7--14, 15--18 and 19 as the
one-texture successor. Thus one lowering helper can populate the complete
destination state for both builders.

## BO lifetime, fences and failure unwind

Reuse the current submit path without weakening its ownership rules. The
one-texture GART order is target, texture, D16; the two-texture order is
target, texture0, texture1, D16. Despite historical internal member names,
the fence usages are respectively:

```text
target   WRITE
texture0 READ
texture1 READ (two-texture only)
D16      WRITE (conservative even when DEPTH_READ_ONLY is set)
```

Lock all three/four reservation objects in one `drm_exec`, wait for all
dependencies, map and pin every object, allocate one contiguous SF node,
install every PTE, prepare/check every leading/L0/trailing guard, flush/sync
all maps, and attach all fences before publishing PID10. A pre-publication
error removes the node and unwinds every map/reference. Once any command can
be fetched, a timeout quarantines the ring segment, SF node, PTE array, all
three/four BO mappings and all object references together until reset. Never
return a source or D16 mapping early because the color target happened to
signal or because alpha test killed the oracle fragment.

The new request kinds must be included in every existing classification:
`textured_depth`, `two_textured_depth`, `three_objects`, `four_objects`,
`texture_objects`, builder selection, fence usage, guard preparation,
CPU/device sync, retirement and quarantine cleanup. Missing one Boolean is a
use-after-unmap or wrong-fence bug, not merely a rejected feature.

## Staged silicon oracles

All tests use distinct native guarded BOs, verify every guard after
retirement, and record submitted/rejected deltas, cookie, CR, work status and
MMIO fault state.

1. **One texture, A33 before D16.** BGRA target starts transparent green and
   D16 `ffff`. REPLACE a `40ff0000` texel at Z=`4000` with GEQUAL/ref `80`
   and ioquake same-factor SRC_ALPHA/INV_SRC_ALPHA: it must be killed and
   leave color/depth unchanged. Then use `80ff0000` at Z=`8000`; expect
   approximately `40807f00` and depth `8000`. Canonical independent-alpha
   source-over would yield alpha `80`, so this also proves arbitrary alpha
   factors instead of the old shortcut.
2. **One-texture length closure.** Repeat the first test with MODULATE and
   white diffuse. It must match REPLACE while exercising the 236-dword body.
   Follow with the old no-blend D16 ioctl, then repeat, proving A00/A33/A52
   closure in both directions.
3. **Two textures, combined product/kill/blend/depth.** Use texture0
   `80ff8040`, texture1 `80ffff80`, LESS/ref `80`, canonical independent-alpha
   source-over, BGRA transparent black and Z=`4000`. The product alpha is
   about `40`, so the covered result is approximately `40402008` with depth
   `4000` (one RGB LSB tolerance). A preceding product with alpha `c0` at a
   nearer Z must be killed without changing color or depth.
4. **Four-object transition.** Run alpha+blend+D16, alpha-disabled COPY+D16,
   then the first draw again; transition BGRA -> BGRX -> BGRA. The two BGRA
   results must match, BGRX must read destination alpha as one, and neither
   texture may inherit the other unit's sampler/image state.
5. **Validation/failure.** For each pair of handles, try aliasing; truncate
   each object in turn; set every reserved/unknown flag; submit an unsupported
   factor; and inject pre-publication and retirement failures. No invalid
   request may publish. A published timeout must quarantine all three/four
   objects and keep every guard/mapping alive.
6. **Live ioquake filter tuple.** Prefill XRGB destination `004080c0` and D16
   `8000`; draw equal/nearer source `00804020` with DST_COLOR,ZERO,
   LEQUAL+READ_ONLY. Expect approximately `00202018` and depth still `8000`.
   Follow it with opaque COPY plus write-enabled LEQUAL, then repeat. This
   jointly proves factor value 8, A01 `...548`, and closure back to `...54c`.

Only after all six pass should Mesa select these ioctls for texture+D16
`blend_successor` state. Keep the old source-over ioctl as a compatibility
fast path; it must not substitute its alpha equation for a different GL
equation on BGRA.
