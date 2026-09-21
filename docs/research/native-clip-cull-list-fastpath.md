# Native clip, cull, and triangle-list fast path

## Result

Chrome9 has enough statically recovered state for a bounded native
clip-space triangle-list experiment, but changing Mesa's vbuf
`need_pipeline()` callback is not that experiment.  Gallium draw always adds
`PT_SHADE`; returning false only removes the primitive pipeline.  A useful
fast path must intercept an accepted draw in `chrome9_draw_vbo()` before
`draw_vbo()`, classify an immutable native VS/PS pair, copy the selected raw
attributes into a kernel-owned immediate list, and let hardware perform the
M4x4, homogeneous clipping, viewport transform, and physical-winding cull.

The first implementation should retain command-mode independent triangle
lists.  Native strips, fans, SF vertex fetch, and inline indices are already
decoded, but they add no clipping or shader capability and materially enlarge
the address/range-validation boundary.  They belong after the immediate
native list is image-correct.

This document reserves private command `0x11` and capability bit 26 for the
multi-triangle successor.  The single-triangle probe remains command `0x10`,
capability bit 24.  Capability bit 25 is independently reserved for
two-texture primary modulation; ioctl ordinals and capability bits are
separate namespaces.  The canonical assignments are in
[chrome9-uapi-allocation.md](chrome9-uapi-allocation.md).

## Independent evidence

The exact binaries are:

```text
via_chrome9_dri.so  fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4
VTGUModeDX32A.dll   8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45
VTGOGL32A.dll       a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c
```

The VX900 manual defines A01 bit 11 as clipping-engine enable, A01 bit 7 as
back-face-cull enable, type-04 A23 bit 8 as the culled physical winding, A23
bits 23/22 as the near-plane triangle-point and W=0 clip disables, and A24 as
the exact vertex count.  It also requires the C-to-screen scale/offset to be
duplicated in A40--A48 and VS constants c256/c257 because the clipping engine
and VS finalizer consume separate copies.

The three executable families agree where an API mapping exists:

- Linux `STM_UpdateCull_INV:0x196ae6..0x196bac` clears/sets A01 byte bit 7
  and type-04 A23 byte bit 0, which is complete-word bit 8.
- Windows D3D `0x101fabd0..0x101face6` and OpenGL
  `0x1024e890..0x1024e9a6` are instruction-identical except for one relocated
  orientation table.  Modes `NONE/CW/CCW = 1/2/3` respectively clear cull,
  enable it with A23[8] clear, and enable it with A23[8] set.  They update
  A23[7], the oBFD selector, independently.
- Windows triangle dispatchers at D3D `0x101f7410` and OpenGL `0x1024be90`
  select byte-matching list builders at `0x101f5350` and `0x10249dd0`.
  Those builders clear A20 `[21:8]`; the manual independently names that
  full three-vertex cycle.
- D3D `SetRenderState` table byte 135 (public `D3DRS_CLIPPING=136`) is
  dispatcher `0x38`.  Its case at `0x100cab06..0x100cab25` stores the value
  at context `+0x1d80` and dirties state class `0x2b`.  Later draw selection
  tests the same field at `0x10110ab9` and `0x101b3976`, and setup at
  `0x1017cea0` changes a path flag.  This proves that vendor clipping was a
  draw-path contract, not that setting A01[11] on the current post-viewport
  XRender program is sufficient.
- Linux H5/H6 exports `STM_Clipping_H5:0x1bab30`,
  `STM_ClipPlaneEnable_H5:0x1bab40`, and
  `STM_SetClipPlane_H5:0x1bab50` as immediate returns.  User clip planes are
  therefore not recoverable as an undocumented toggle.  Keep them in
  software until native clip-distance output has its own compiler oracle.

Old Mesa 7.11.2 is behavioral precedent, not Chrome9 encoding evidence.  Its
TNL path performs software clipping before rendering, enables the older
generation's `HC_HenFBCull_MASK`, and resolves GL front/back plus `FrontFace`
to physical winding in `viaChooseTriangle()`.  A Chrome9 native fast path must
preserve that API-to-physical separation.

## Exact packet state

Use the proven perspective state image as the base.  The only allowed A01
deltas for the first oracle are clip bit `0x800`, cull bit `0x80`, and the
already established D16 test/write bits `0x0c`:

| clip | cull | D16 test/write | complete A01 |
|---:|---:|---:|---:|
| 1 | 0 | 0 | `01ce1d40` |
| 1 | 1 | 0 | `01ce1dc0` |
| 1 | 0 | 1 | `01ce1d4c` |
| 1 | 1 | 1 | `01ce1dcc` |

Do not form those words by ORing the last packet image.  Rebuild A01 from the
closed template on every segment so a preceding legacy draw cannot leak cull,
depth, clipping, or perspective state.

The primitive block is exact:

```text
A20 = 20000000                         # command-mode independent list
A23 = 23004f02                         # Gouraud; cull physical CCW if enabled
A23 = 23004e02                         # Gouraud; cull physical CW if enabled
A24 = 24000000 | (3 * triangle_count) # vertices, not payload dwords
```

A23 bits 23 and 22 must remain zero.  Thus near clipping for triangle-point
mode and the W=0 clipping ECO both remain enabled.  Bit 7 must retain the
established linkage image; it selects oBFD and is not part of the public cull
enum.  `PIPE_FACE_FRONT_AND_BACK` is a userspace no-op draw, not a fourth
hardware enum.

The compact H6 declarations and immediate strides remain the tables from the
single-triangle oracle:

| native VS | H6 declaration | stride dwords |
|---|---|---:|
| position | `00000004 010000c2` | 4 |
| position + T0 | `00000106 010000c2 02004142` | 6 |
| position + T0/T1 | `00000208 010000c2 02004142 03006242` | 8 |
| position + COL0 + T0 | `00000207 010000c2 020041d1 03005242` | 7 |
| position + COL0 + T0/T1 | `00000309 010000c2 020041d1 03005242 04007342` | 9 |

Packed color format `d1` is mandatory.  The live blend discriminator proved
that `d0` supplies byte values as unnormalized floats and clamps ordinary
nonzero alpha to one.

For `n=3*T` vertices of stride `s`, the final packet is:

```text
FE020440 0000043C 00000000 CC000000
<n * s compact input dwords>
<(-n * s) mod 4 copies of CC000000>
```

There is no explicit fire or PLEND dword.  A24 terminates the list.  Every
segment must assert `body_dw = fixed_dw[key] + n*s + pad_dw`, the exact PAUSE
and STOP positions, and `stop_byte + 0x220 <= command_staging_size`.  The
maximum per segment is consequently a generated immutable table indexed by
VS, PS, texture/depth state; it must not reuse the post-TNL value 34 without
this proof.  The public list can be split into several complete segments,
with full state and alternating completion cookie in each segment, while one
resource mapping and one reservation fence cover the whole synchronous run.

The native program, A25/A26/A28 linkage, A61/A62/A60 controls, finalizer,
PS program, and c0--c3 plus c256/c257 constants are indivisible table entries
from [native-clip-vs-uapi-oracle.md](native-clip-vs-uapi-oracle.md).  Do not
mix a Linux semantic prefix with a Windows finalizer.  The first draw still
requires that command `0x10`'s address-free program and activation probes
passed for the exact VS/PS key.

## Bounded list ABI

Private command `0x11` is a synchronous successor, not a raw shader or command
submission ABI:

```c
#define DRM_CHROME9_DRAW_CLIP_TRIANGLE_LIST       0x11
#define DRM_CHROME9_CAP_DRAW_CLIP_TRIANGLE_LIST   (1ULL << 26)
#define DRM_CHROME9_CLIP_LIST_MAX_TRIANGLES       128U

struct drm_chrome9_draw_clip_triangle_list {
        __u32 vs_template;             /* 0 */
        __u32 fs_template;             /* 4 */
        __u32 flags;                   /* 8: closed depth/texture flags */
        __u32 triangle_count;          /* 12: 1..128 */
        __u32 destination_handle;      /* 16 */
        __u32 depth_handle;            /* 20: zero unless D16 */
        __u32 width;                   /* 24 */
        __u32 height;                  /* 28 */
        __u32 destination_pitch;       /* 32 */
        __u32 depth_pitch;             /* 36 */
        __u32 destination_format;      /* 40 */
        __u32 depth_func;              /* 44 */
        __u32 physical_cull;           /* 48: NONE/CW/CCW */
        __u32 reserved0;               /* 52: zero */
        struct drm_chrome9_scissor_state scissor; /* 56..67 */
        __u32 pad;                     /* 68: zero */
        struct drm_chrome9_texture_state textures[2]; /* 72..143 */
        __u32 matrix[16];              /* 144..207, c0..c3 */
        __u64 vertices_ptr;            /* 208 */
        __u64 reserved[5];             /* 216..255: zero */
};
```

The request is exactly 256 bytes on 32- and 64-bit userspace.  Vertex records
are the existing fixed 36-byte clip records `(x,y,z,w,color,s0,t0,s1,t1)`;
the kernel copies at most `128*3*36 = 13,824` bytes with
`memdup_array_user()`, validates all records, then packs only the fields used
by the selected template.  Unknown flags, nonzero padding, invalid enum
values, template/output mismatches, aliases, imported/unguarded objects, and
noncanonical pitches fail before lookup or publication.

The first implementation accepts XRGB/BGRA targets already proved by the
selected PS path, optional linear D16 LESS/LEQUAL test/write, zero or the
required one/two guarded level-zero textures, no blend, no alpha test, and no
polygon offset.  Those exclusions prevent the first native-geometry gate from
also becoming a new raster-state experiment.

Identity c0--c3 remains the first silicon gate.  The performance successor
may accept a matrix only after that gate, with a bitwise kernel validator:

- every input position and matrix coefficient is finite, non-NaN, and has
  absolute value at most `16.0f` (`abs_bits <= 0x41800000`);
- signed zero is accepted; denormals are initially rejected;
- required colors and texture coordinates obey their existing template
  validators; unused fields are zero.

Those bounds guarantee each four-term dot product is finite and at most 1024
in magnitude without using floating point in the kernel.  They do not require
positive W: mixed-sign and W=0 primitives are precisely clipping-engine cases,
but they remain disabled until the dedicated W-plane oracle passes.  Before
that promotion, require input `w=1` and the identity matrix exactly.

One `drm_exec` transaction, one job/fence, one SF mapping, and one ownership
round trip cover the list.  Each segment alternates `00010001/00020002`, waits
for cookie, PAUSE, and non-CR idle, and updates `last_cookie` immediately.
Validation errors publish nothing.  A clean image mismatch quarantines the
exact native key; timeout, engine error, guard mutation, or unexpected busy
state quarantines all native-clip/list keys until module reload.

## Mesa bypass gate

The direct path belongs at the start of `chrome9_draw_vbo()`, after sampler
shadow refresh and ordinary zero-count/state checks but before mapping state
into Gallium draw.  `chrome9_create_vs_state()` must retain/classify NIR in
addition to building the software draw shader.  The first classifier accepts
only an immutable template and falls back to the existing draw path on every
mismatch.

Initial draw restrictions are deliberately strict:

- `MESA_PRIM_TRIANGLES`, count divisible by three, one instance, direct draw,
  no primitive restart, no draw-id/base-instance semantics, and initially no
  API index buffer;
- fill mode on both faces, no polygon stipple, no line/point conversion,
  rasterizer discard off, Gouraud interpolation, and no user clip distances;
- an exact accepted VS (`MVP_POSITION`, `MVP_*_T0`, `MVP_*_T0_T1`, or the
  corresponding COL0 form), an accepted fixed PS, and matching vertex
  elements/constant slots;
- complete checked CPU ranges for every selected attribute, including
  `src_offset + (first + count - 1)*stride + format_size`;
- physical cull `NONE/CW/CCW` resolved in Mesa after framebuffer/viewport Y
  orientation.  Until the paired winding oracle fixes that mapping, force
  cull-none and keep the software path for culling applications.

Returning false from `chrome9_vbuf_need_pipeline()` is insufficient: the
draw middle end still shades vertices (`PT_SHADE`) and only omits clipping,
culling, optional raster stages, and topology decomposition.  Ordinary
filled flat shading is not automatically fixed even with the full pipeline;
it needs the explicit provoking-color path.  Conversely, pointing native
fetch at raw application data is invalid because the proven post-TNL
declaration consumes float4 position plus normalized packed colors, not
arbitrary float3 positions/normals.  The fast path must either pack the
bounded immediate record above or use a separately verified native input
declaration.

This first path can help ioquake-style MVP+COL0+T0/T1 lists after the matrix
gate.  It does not yet accelerate glmark2's Gouraud cat shader: that draw has
raw position and normal inputs and a lighting VS.  The recovered 20-record
fixed-lighting compiler vector is an ISA oracle, not yet a byte-for-byte
classifier for that NIR.  Keep cat on software draw until position/normal
input linkage, material/light constants, scalar varying output, and the exact
native finalizer have a closed corpus and address-free activation probe.

## Later native fetch and indexing

After the immediate list passes, an address-bearing VBO successor may use the
already decoded protocol:

```text
FE03043C 00000001 30030000 CC000000
<1-KiB-aligned SF VA | (pitch >> 2)> CC000000 CC000000 CC000000

A20 = 20000001  # 16-bit inline indices
A20 = 20000003  # 32-bit inline indices
A2C = 2c000000 | max_index_low24
A2D = 2d000000 | min_index_low24
A2E = 2e000000 | max/min high fields
```

The kernel computes min/max after bias, checks every element's final byte
against every pinned BO with checked arithmetic, derives stream/address-low
bits and formats from an enum, and copies indices inline.  A final unused
16-bit lane is zero.  Primitive restart is split in userspace or the kernel;
it is never exposed to unknown hardware behavior.  Native index mode reduces
duplicate vertex payload but still copies the index packet, so it should be
profile-driven after the clipping/VS win is measured.

## Silicon validation sequence

Every hardware step uses distinct guarded color/depth/texture objects, exact
full-page reference hashes, cookie and PAUSE checks, non-CR idle, `040c=0`,
and a legacy 2D fill closure.  Invalid fetches and invalid native programs are
validator tests only and are never intentionally published.

1. Run command `0x10`'s address-free program and A60 activation probes, then
   its identity inside triangle with clipping disabled.  Reproduce it with
   clipping enabled and cull-none; require byte-identical output.
2. Cross each D3D homogeneous plane separately with one outside vertex:
   `x=-2w`, `x=+2w`, `y=-2w`, `y=+2w`, `z=-0.5w`, and `z=1.5w`, keeping the
   other two vertices inside.  Compare against triangles generated by a
   deterministic CPU clipper and sent through the already proven post-TNL
   path.  Test position-only first, then COL0 and T0 interpolation.
3. Put all three vertices beyond each plane and require a byte-identical
   prefill with clean retirement.  Only after all six pass, test the manual's
   special positive-Z, mixed-positive/negative-W case while A23[22] remains
   zero.  W=0 itself is last.
4. With all vertices inside, draw one ordered triangle and its `v1/v2`
   reversal.  Under each A23 selector exactly one image may change.  Record
   physical hardware winding before deriving Gallium `front_ccw`; repeat with
   the active Y orientation and then with a triangle crossing one clip plane.
5. Draw two independent triangles in one A24 list, then list sizes one below,
   at, and one above every generated segment capacity.  Require identical
   images and prove cookie alternation across the split.
6. Exercise closure sequences `clip+cull -> legacy RHW cull-off -> opposite
   cull`, `native D16 -> legacy D16`, and every accepted VS/PS/texture-count
   key.  No state field may be inherited.
7. Unit-test zero/oversized counts, multiplication overflow, NaN/Inf/denormal
   inputs, coefficient `>16`, bad enum/template pairs, short buffers, aliases,
   noncanonical sizes/pitches, and nonzero reserved data.  Assert that none
   reaches PTE publication or advances the command ring.

Only after these gates may Mesa prefer the native path.  Capability exposure
still does not imply general GLSL, arbitrary shaders, user clip planes,
two-sided lighting, or native VBO/index support.
