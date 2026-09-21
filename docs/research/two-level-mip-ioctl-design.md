# Versioned one-texture two-level mip ioctl design

This is the implementation-ready, deliberately narrow successor to the live
level-zero one-texture ioctl.  It adds exactly one reachable successor level
to the existing color-only REPLACE/MODULATE draw.  It does not combine mip
sampling with blend, D16, two textures, NPOT, imported mip chains, L9--L11,
or a programmable shader.

The packet fields are high-confidence static evidence from both Windows UMDs,
the proprietary Linux UMD, and the VX900 manual.  The complete two-level
packet has not yet run on silicon.  The capability must remain absent until
the bounded oracle below passes without timeout, guard damage, or stale state.

## Evidence that fixes the hardware encoding

The independent paths agree as follows:

| state | Linux `via_chrome9_dri.so` | Windows D3D | Windows OpenGL |
|---|---:|---:|---:|
| image and eight-entry table | `stmHWPrepareTextureRegs_invi:0x1b251d` | `0x101d7c10` | `0x10240740` |
| LOD range and signed bias | `STM_MipMapLOD_INV:0x1b1a50` | `0x101d9a40` | `0x10242260` |
| spatial and mip filters | recovered Linux sampler builder | `0x101d9380` | `0x10241ba0` |

Both Windows image builders accept up to twelve resource levels, cap the
explicit table to nine total levels, write L1--L8, and clear every unused
low-24 address field.  Linux performs the same cap and relative subtraction
at `0x1b264a..0x1b26ad` and `0x1b2ab1..0x1b2b45`.  The Windows A30 packing is
byte-identical at `0x101d9b7b..0x101d9ba4` and
`0x1024239b..0x102423c4`; Linux matches it at
`0x1b1b29..0x1b1b69`.

This establishes the fixed eight-word table and these exact semantic words:

```text
A30 = 30000000
    | ((signed_bias_s5_5 & 03ff) << 12)
    | ((maximum_level & 3f) << 6)
    |  (minimum_level & 3f)

A31 = 31100000
    | (mag_filter << 13) | (min_image_filter << 10)
    | (mag_filter <<  7) | (min_image_filter <<  4)
    | mip_filter
```

The old Mesa VIA path independently truncates `float_bias * 32` and clamps it
to `[-512,511]`.  Userspace should perform that API conversion; the kernel
accepts only the already-converted signed s5.5 integer.

## New UAPI, not reinterpretation of an old field

At the current header snapshot, ioctl command `0x0e` and capability bit 21
are the next unused values:

```c
#define DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP       0x0e
#define DRM_CHROME9_CAP_DRAW_TEXTURED_TRIANGLE_MIP   (1ULL << 21)

#define DRM_IOCTL_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP                 \
        DRM_IOW(DRM_COMMAND_BASE +                                  \
                 DRM_CHROME9_DRAW_TEXTURED_TRIANGLE_MIP,             \
                 struct drm_chrome9_draw_textured_triangle_mip)

#define DRM_CHROME9_TEXTURE_MIP_FILTER_NONE          0
#define DRM_CHROME9_TEXTURE_MIP_FILTER_NEAREST       1
#define DRM_CHROME9_TEXTURE_MIP_FILTER_LINEAR        2

struct drm_chrome9_draw_textured_triangle_mip {
        __u32 destination_handle;        /* byte offset 0 */
        __u32 texture_handle;            /* 4 */
        __u32 flags;                     /* 8, must be zero */
        __u32 program;                   /* 12, REPLACE or MODULATE */
        __u32 width;                     /* 16 */
        __u32 height;                    /* 20 */
        __u32 destination_pitch;         /* 24 */
        __u32 texture_width;             /* 28, L0 */
        __u32 texture_height;            /* 32, L0 */
        __u32 texture_pitch;             /* 36, canonical L0 */
        __u32 texture_format;            /* 40, BGRX/BGRA8888 */
        __u32 min_filter;                /* 44, spatial nearest/linear */
        __u32 mag_filter;                /* 48, spatial nearest/linear */
        __u32 wrap_s;                    /* 52 */
        __u32 wrap_t;                    /* 56 */
        __u32 texture_last_level;        /* 60, must equal 1 initially */
        __u32 mip_filter;                /* 64, NONE/NEAREST/LINEAR */
        __s32 lod_bias_s5_5;             /* 68, -512..511 */
        __u32 reserved0;                 /* 72, must be zero */
        struct drm_chrome9_textured_vertex vertices[3]; /* 76..171 */
        struct drm_chrome9_scissor_state scissor;       /* 172..183 */
        __u32 reserved[2];                /* 184..191, must be zero */
};
```

The structure is exactly 192 bytes with no architecture-dependent pointer or
padding.  Add a compile-time size assertion.  The new command number itself
versions the ABI; no old `pad` or `reserved` member changes meaning.  If an
intervening UAPI consumes command `0x0e` or capability bit 21, reassign both
before implementation rather than creating an alias.

The first capability does not expose arbitrary minimum/maximum levels.
Minimum is zero and maximum is exactly one.  A later view-range successor can
add semantic first/last levels; it must not overload `lod_bias_s5_5` as a
view selector.

The mechanical kernel integration points are bounded: add one draw-kind enum
and the three mip fields to the internal request; add the 192-byte static
assert and handler prototype; register one `DRM_RENDER_ALLOW` ioctl beside the
existing textured ioctls; classify the new kind as a two-object texture-source
job; and add the capability to `GET_PARAM` only after the raw live gate.  The
handler should reuse the old vertex/scissor validation and call the same
submission function.  It must not duplicate mapping, fence, retirement, or
quarantine code.

## Fail-closed validation

Reject before BO pinning or command construction unless every condition is
true:

- flags and all reserved fields are zero;
- destination and texture handles resolve to distinct, native Chrome9 shmem
  objects; cross-device/import-attachment objects remain rejected;
- destination dimensions are `1..2048`, its BGRX pitch is exactly
  `ALIGN(width*4,32)`, and its object has the exact existing guarded size;
- texture width and height are powers of two in `2..2048`, format is only
  BGRX8888 or BGRA8888, and L0 pitch is exactly
  `ALIGN(texture_width*4,32)`;
- `texture_last_level == 1`, `mip_filter <= LINEAR`, both spatial filters are
  nearest/linear, wrap S/T use the existing three-value set, and bias lies in
  `[-512,511]`;
- program is only the established REPLACE or MODULATE object;
- every vertex, RHW, S/T, triangle, scissor and destination check from the
  old ioctl passes unchanged; and
- checked arithmetic derives the exact complete texture object below, total
  mapped bytes fit the existing 64 MiB SF window, and the GEM object size
  equals that complete size.  Neither a shorter old L0 object nor an
  arbitrary larger BO is accepted.

The POT restriction is an implementation containment boundary, not a claimed
hardware limitation.  It avoids simultaneously testing NPOT LOD, mip layout,
and table addressing.

## Exact current guarded layout derivation

Use the layout already allocated by current Mesa, not the vendor-packed
alternative:

```text
P          = PAGE_SIZE
l0_pitch   = ALIGN(width0 * 4, 32)
l0_size    = l0_pitch * height0
l0_base    = P
l0_object  = P + ALIGN(l0_size, P) + P

width1     = width0 >> 1
height1    = height0 >> 1
l1_pitch   = ALIGN(width1 * 4, 32)
l1_size    = l1_pitch * height1
l1_offset  = ALIGN(l0_object, 256)
complete   = ALIGN(l1_offset + l1_size, P) + P

l1_relative = l1_offset - l0_base
l1_entry    = 40000000 | (l1_relative >> 8)
```

Validate that `l1_relative` is 256-byte aligned and fits the table's low 24
bits.  No offset, L1 pitch, object size, or native word comes from userspace.

For the first 32x32/16x16 oracle:

```text
l0_pitch=0080 l0_size=1000 l0_base=1000 l0_object=3000
l1_pitch=0040 l1_size=0400 l1_offset=3000 complete=5000
l1_relative=2000 l1_entry=40000020
```

The vendor-packed `40000010` is valid only for a different allocation and
must not be emitted for this object.

## Submission ownership and guard invariants

The current two-object texture submission internally calls the texture BO
`depth_obj`.  A mip kind must preserve that established ownership path while
making its two sizes explicit:

```text
request.depth_object_size   = l0_object     # end of retained L0 guard
request.texture_object_size = complete      # exact actual texture BO
```

The object check for the mip kind must require
`depth_obj->size == request.texture_object_size`.  Map and collect PTEs for
the complete object.  Prepare/verify guards at both `l0_object` and
`complete`: for the example those are the internal page `0x2000..0x2fff`
and final page `0x4000..0x4fff`, with the common leading page at
`0x0000..0x0fff`.  Cache transfer and flush cover the complete BO.

The texture reservation is read usage and the destination is write usage.
Both objects and every SF PTE remain owned by the published job until bounded
retirement.  A timeout follows the existing wedge/quarantine path and must
retain the complete texture, not unwind it merely because userspace receives
an error.

The texture address passed to the builder remains:

```text
job->sf_node.start + destination_obj->size + PAGE_SIZE
```

It points at L0.  The H3 entry is relative to that address, which is why the
object-relative L1 offset `0x3000` becomes relative `0x2000`.

## Exact packet insertion

Build an eight-word payload every mip draw:

```text
level_table[0] = l1_entry
level_table[1..7] = 00000000
```

After the six-word image payload and before the sampler, call the existing H3
emitter with selector `10030000` and count eight.  For the color-only builder
body the exact sequence is:

| body index | word |
|---:|---|
| `20` | image A00, texture L0 SF address `>>8` |
| `21` | `18001101` |
| `22` | exact L0 A20 dimensions |
| `23` | `21000001` |
| `24` | A22 POT/log2 word |
| `25` | image A30 format word |
| `26..27` | alignment poison/pad |
| `28` | `FE03043C` |
| `29` | `00000008` |
| `30` | `10030000` |
| `31` | `CC000000` |
| `32` | `40000020` for the 32x32 oracle |
| `33..39` | zero L2--L8 entries |
| `40..43` | sampler H2 header, selector `20020000` |
| `44` | sampler A30 |
| `45` | sampler A31 |
| `46` | sampler A32 |
| `47` | sampler pad |
| `48..51` | following common-state H2 header |

The mip successor uses:

```text
sampler[0] = 30000040 | ((lod_bias_s5_5 & 03ff) << 12)
sampler[1] = 31100000
           | (mag_filter << 13) | (min_filter << 10)
           | (mag_filter <<  7) | (min_filter <<  4)
           | mip_filter
sampler[2] = 32000040 | (wrap_t << 3) | wrap_s
sampler[3] = CC000000
```

Thus useful exact combinations are:

| request | A30 | A31 |
|---|---:|---:|
| nearest spatial, no mip selection, bias 0 | `30000040` | `31100000` |
| nearest spatial, no mip selection, bias +1 | `30020040` | `31100000` |
| nearest spatial and nearest mip | `30000040` | `31100001` |
| bilinear spatial and nearest mip | `30000040` | `31102491` |
| trilinear, bias 0 | `30000040` | `31102492` |
| trilinear, bias +0.5 | `30010040` | `31102492` |

The H3 block adds exactly 12 dwords.  With no blend or depth, the body is 236
dwords for REPLACE and 240 for MODULATE.  The final immediate-vertex H2 header
moves from body index 196 to 208 for REPLACE and from 200 to 212 for MODULATE.
Assert body length, H3 header/count/selector/entry, A30/A31/A32, and the final
immediate header before publication.  The command remains comfortably inside
the existing one-page staging allocation.

The old level-zero ioctl continues writing A30=`30000000` and its complete
A31/A32 state.  It need not clear the stale H3 table because its min/max and
D-filter make every successor level unreachable.  The mip successor must
nevertheless rewrite all eight table entries on every draw.

## Immediately runnable bounded oracle

Use the existing raw DRM smoke path so shader classification and Gallium
resource-copy support are not prerequisites.  Allocate the exact `0x5000`-
byte texture above and a distinct ordinary guarded 32x32 destination.

Initialize:

- every L0 logical texel to opaque red;
- every L1 logical texel to opaque green;
- L0/L1 row padding and `0x3400..0x3fff` allocation padding to distinct poison
  colors;
- leading, internal and final guard pages through the kernel's normal guard
  preparation, never as texture data; and
- nearest/clamp REPLACE with safely interior coordinates for address gates.

Run in this order:

1. **Old-ABI closure:** old level-zero ioctl, A30=`30000000`; require red.
2. **Table present but D-filter off:** mip ioctl, bias 0 and mip NONE,
   A30/A31=`30000040/31100000`; require red.
3. **Address L1 without derivatives:** bias `+32` s5.5 and mip NONE,
   A30/A31=`30020040/31100000`; require green.  This isolates bias plus the
   `40000020` table address from derivative selection.
4. **Nearest derivative selection:** bias 0, nearest spatial/mip,
   `30000040/31100001`; map 32 source texels onto 16 destination pixels and
   require green at interior pixels, then render 1:1 and require red.
5. **Trilinear midpoint:** bilinear spatial, linear mip, bias `+16` s5.5,
   `30010040/31102492`; at 1:1 require red and green channels independently
   in `{0x7f,0x80}`, blue zero.
6. **Transition closure:** forced L1 through the mip ioctl, old level-zero
   draw, forced L1 again; require green/red/green without reinstalling the
   context or resetting the engine.

After every submission verify destination pixels, L0 and L1 contents, all
row/allocation padding, all three guard pages, retirement cookie, CR PAUSE,
and clear engine work bits.  Any timeout is a quarantine/reboot event, not a
pixel mismatch.

Before the first valid command, issue negative tests for a non-POT dimension,
`last_level != 1`, bad mip filter, bias outside `[-512,511]`, noncanonical L0
pitch, shorter old-L0-only BO, oversized BO, aliased destination/texture,
external imported object, nonzero reserved field, and invalid vertex/scissor.
Each must return before publication, leave submission counters unchanged, and
leave the engine idle.

Once the raw sequence passes, wire Mesa only for a locally allocated guarded
POT L0--L1 view.  The ioquake NULL `resource_copy_region` repair can populate
levels through the already level-aware transfer path, but it does not prove
hardware mip selection.  Likewise the projected ioquake shader is an
independent program gate; this first mip oracle deliberately uses the already
live REPLACE object.
