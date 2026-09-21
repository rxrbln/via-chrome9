# ioquake depth-only `ALWAYS` plus color-mask-zero draw

Status: exact live application state and packet-ready extension, 2026-09-21.
The ioquake draw reached the Gallium state gate and was intentionally rejected
before ioctl; the engine remained idle and clean. Depth `ALWAYS` is exact in
the manual and three proprietary builders but this combined tuple has not yet
run on VX900. Capability bit 35 is reserved for the bounded promotion.

## Live state, not a GPU failure

`/root/tmp/chrome9/ioquake3-gl1-equal-v1.log` captured:

```text
chrome9: triangle rejected by bounded draw state: reason=depth-state fs=6
layout=1 tex=0 views=1 samplers=2 target=281 depth=268 dsa=1/1/7
stencil=0/0 blend=0/0/0/0/0/0/0 mask=0 blend_successor=1
caps=d16:1 d16b:1 tex:1 texb:1
```

The exact interpretation is:

| field | meaning |
|---|---|
| `fs=6` | `CHROME9_FS_CONSTANT_COLOR` |
| `layout=1`, `tex=0` | valid untextured post-TNL vertex layout |
| target `281` | `PIPE_FORMAT_B8G8R8X8_UNORM` |
| depth `268` | `PIPE_FORMAT_Z16_UNORM` |
| `dsa=1/1/7` | depth test enabled, writes enabled, `PIPE_FUNC_ALWAYS` |
| `stencil=0/0` | both stencil faces disabled |
| `blend=0/...` | blending disabled, canonical unused factors/functions |
| `mask=0` | no RT0 channels may be written |
| `blend_successor=1` | color-mask state requires the blend-aware ioctl |

The stale-looking `views=1 samplers=2` is not an input to this draw. The
classified fragment program has zero texture operations and `texture_count`
is zero, so neither sampler nor view is placed in the request. Requiring all
unrelated bindings to be empty would be a false rejection and would not make
the packet safer.

The engine status after rejection was `0400=0`, `040c=0`. This is a missing
state promotion, not a hang, VM fault, resource-lifetime problem, or shader
compiler failure. The draw must not be optimized away merely because the
color mask is zero: its purpose is the enabled D16 write.

## Independent encoding evidence

The VX900 manual defines A13 bits `[18:16]` as the ordered eight-way depth
comparison and type-01 A01 bits 3/2 as depth-test/depth-write enable. The
three proprietary paths agree:

* Linux `via_chrome9_dri.so`, SHA-256
  `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`,
  `STM_ZFunc_INV:0x192cf0` and `STM_ZWBEnable_INV:0x195b90`;
* Windows D3D `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
  depth function `0x101faa10` and enable/write `0x101fa910`; and
* Windows OpenGL `VTGOGL32A.dll`, SHA-256
  `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`,
  depth function `0x1024e6d0` with the byte-equivalent state builder.

Each function path converts the public `1..8` enum to native `0..7` and
places it directly at A13 `[18:16]`. Therefore `ALWAYS` is exactly:

```text
A13 = 13070000
```

The same sources independently keep test and write enabled even for ALWAYS.
Do not replace it with “depth disabled plus a write”: A01 bit 2 is not a
standalone unconditional-write mode, and the recovered drivers clear write
when test is disabled. The complete control is:

```text
A01 = 01c0154c   # bit 3 depth test, bit 2 depth write
```

A53 is the render-target channel gate. Its low bits are physical B/G/R/A,
while the public mask is R/G/B/A. Zero is invariant under that remap:

```text
A53 = 53010c00   # no destination channel writes
```

Keep A01 color-enable bit 6 set. The existing mask path is the bounded
mechanism; clearing the global color bit would introduce a second, unproved
depth-only mode. A-only masking is already live on VX900, and the manual plus
proprietary state image establish mask zero. What remains to prove is its
interaction with ALWAYS and a D16 update.

## Exact existing ioctl representation

No new request or field is needed. Use the 144-byte
`drm_chrome9_draw_triangle_d16_blend` request at command 08, requiring base
capability 12:

```text
flags                  = DRM_CHROME9_DEPTH_FUNC(ALWAYS) = 00000700
destination_format     = BGRX8888
blend.flags            = 0
blend.rgb_src_factor   = ONE
blend.rgb_dst_factor   = ZERO
blend.alpha_src_factor = ONE
blend.alpha_dst_factor = ZERO
blend.rgb_func         = ADD
blend.alpha_func       = ADD
blend.color_mask       = 0
blend.alpha_test_func  = 0
blend.alpha_test_ref   = 0
pad/reserved[]         = 0
```

The disabled equation fields are still canonical rather than don't-care.
This satisfies the existing blend validator and prevents caller-controlled
garbage from entering future transitions. The destination and depth handles
remain distinct retained guarded objects under one reservation transaction
and fence. The depth object remains WRITE. The target also remains WRITE in
the resource contract even though its channel mask is zero; narrowing that
reservation based on raster state is an unnecessary first-test hazard.

The current three-vertex state-complete body is exactly 192 dwords. For this
draw, its closed first group is:

| body | state | exact value |
|---:|---|---:|
| 4 | A50 | target SF address, kernel generated |
| 6 | A52 | `52980000 | destination_pitch/32` |
| 7..14 | A38..A3F | `38000000` through `3f000000` |
| 15 | A34 | `34000150` |
| 16 | A35 | `35000114` |
| 17 | A36 | `36000590` |
| 18 | A37 | `37000224` |
| 19 | A86 | unchanged established state |
| 20 | A87 | unchanged established state |
| 21 | A53 | `53010c00` |
| 22 | A00 | `00000001`, blend/alpha test disabled |
| 23 | A01 | `01c0154c`, D16 test and write |
| 24..25 | A88/A89 | unchanged established state |
| 26 | A10 | depth SF address, kernel generated |
| 27 | A11 | `11000001 | ((depth_pitch/32)<<13)` |
| 28 | A12 | `12380000`, linear D16 |
| 29 | A13 | `13070000`, ALWAYS |
| 30 | A20 | `20007fff`, wide-open pixel-Z clip |
| 31 | pad | `cc000000`, alpha test disabled |

The next H2 header remains body 32; the scissor block remains 108..111; the
immediate H2 header remains 168..171; three six-dword vertices occupy
172..189; and padding remains 190..191. The constant-color fragment result is
carried through normalized packed D1 in the existing path, but A53 suppresses
all four stores. No PS, FVF, vertex-stride, packet-length, relocation, or
retirement change is required.

## Capability allocation and validation

Use the canonical ledger's next free bit:

```c
#define DRM_CHROME9_CAP_DEPTH_ALWAYS (1ULL << 35)
```

Bit 15 retains the baseline validated depth-function subset, bit 32 adds
EQUAL, and bit 35 adds ALWAYS. Capability 35 consumes no ioctl ordinal and no
request flag: it says that the existing value 7 in depth bits `[10:8]` is
accepted. Userspace must require capability 35 and the selected draw's base
capability; this tuple specifically requires caps 12 and 35. Capability 9 is
needed only for a BGRA destination.

The kernel validator may add only `DRM_CHROME9_COMPARE_ALWAYS` to the accepted
set. It must continue rejecting NEVER, GREATER, NOTEQUAL, GEQUAL, unknown
flag bits, noncanonical blend-disabled factors, invalid masks, nonzero
reserved fields, aliases, bad pitches/sizes, invalid vertices, and
degenerate triangles. Advertise bit 35 only when every structured D16 ioctl
that shares the validator emits `13070000` and retains the correct BO
lifetime, or split the capability if acceptance is intentionally narrower.

Mesa maps `PIPE_FUNC_ALWAYS` to the already matching value 7 only when cap 35
is present. Mask zero sets `blend_successor=true`, so an untextured draw must
select command 08 rather than the legacy command 04. The shader constant must
still be finite and packable by the existing constant-color path even though
the mask makes it store-inert. This preserves one validation model across
mask transitions.

## Discriminating live oracle

The first raw test must distinguish ALWAYS from both LESS and a missing depth
write in one draw:

1. CPU-fill XRGB target with `44332211` and D16 with `4000`; poison all four
   guard pages.
2. Draw an interior triangle at incoming Z=`c000`, opaque red, mask zero,
   blend/alpha disabled, `ALWAYS` plus writes.
3. Covered color must remain exactly `44332211`; covered depth must become
   exactly `c000`. Uncovered color/depth and every guard remain unchanged.

Incoming `c000` fails LESS against `4000`, so a stale LESS word cannot fake
the result. A color-mask leak changes `44332211`; disabled/read-only depth
leaves `4000`. Follow with a full-mask green LEQUAL draw at Z=`c000`: it must
store green without changing the already-equal depth. If the first draw did
not write, `c000 <= 4000` fails. This proves that the invisible first draw
really updated D16 and that mask-zero state did not poison the next draw,
without making the cap-35 oracle depend on the separately versioned EQUAL
capability.

Then run closure in one context:

```text
ALWAYS/write + mask0
LESS/write   + full mask
ALWAYS/write + mask0
legacy color COPY with no depth
```

Reset resources between compare discriminators, verify exact target and D16
contents, all guards, retirement cookie, CR PAUSE, work-mask clear, and
`040c=0`. Any timeout or guard mutation invokes the existing quarantine path;
an image mismatch alone does not justify widening the compare set.

## Cull and stencil states likely to follow

This live draw has stencil disabled, so adding stencil cannot help it. The
current Gallium vbuf path always requests the draw pipeline, which performs
clipping, physical-orientation culling, flat-shade fixup, and decomposition
before emitting independent triangles. Keep hardware culling disabled for
this promotion. ioquake back-face culling is therefore already semantically
handled without a new kernel field; a native A01/A23 cull cap is a later
performance path and must not be coupled to depth ALWAYS.

Stencil is the next materially different state, but it cannot be added to
the D16 request. The recovered hardware contract requires a retained D24S8
surface (`A12=123e0000`), A01 stencil bit 4, and complete physical-CW/CCW
A23--A26 blocks. Mesa currently discards `set_stencil_ref` and rejects either
enabled stencil face, so a safe successor requires all of the following as
one versioned feature:

* guarded D24S8 allocation, CPU clear/readback, and format-aware pitch/size;
* stored dynamic front/back reference bytes;
* Gallium operation remap `{0,1,2,3,4,6,7,5}` rather than raw enum copy;
* API-front/back to physical-CW/CCW resolution after framebuffer orientation;
* complete disabled-state A23--A26 closure; and
* compare-mask, write-mask, Z-fail, operation-wrap, opposite-winding, and
  transition oracles.

Until that is live, keep stencil formats/capabilities unavailable and reject
the draw before publication. Cull remains software and D16 remains explicitly
stencil-free. This ordering isolates the current one-word A13 promotion from
new resource formats and four additional stencil state words.
